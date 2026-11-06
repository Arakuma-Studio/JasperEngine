# Export pipeline: exports never ship wrong silently (design)

Date 2026-11-04. Branch `dc/export-pipeline`. Program: Doriax catch-up, ledger
`docs/doriax-catchup/LEDGER.md` (feature 3). Dossier:
`dc-work/dossiers/export-pipeline.json` (19 Doriax commits). Base: `main` at `b4f7816ea`.

## 1. Goals

1. Every file an export copies or writes either lands or fails the export, naming the file and the
   reason: generated sources, assets, Lua, C++ scripts and headers, shader sources, the engine
   copy, native files, notices, web and desktop artifacts. This holds for every mode (source,
   Desktop/Web Compile, prebuilt Player). A folder that cannot be read is named in a warning.
2. No export contains another export or IDE state. Every front end (Build Settings, CLI, MCP)
   refuses an output folder inside the project. Earlier exports still inside the project are left
   out and named. IDE output in the engine SDK's workspaces never ships. The user's own IDE state
   in an exported workspace (Android Studio, Xcode) survives a re-export.
3. A build ships exactly the shader variants it needs:
   - saved keys of deleted forks are dropped, and split-stage (`vert|frag`) forks keep theirs;
   - the variants of objects scripts create during Play are recorded at Stop, including those of
     scenes a load replaced;
   - one function computes the build's key set for the exporter, Build Settings, MCP and the
     standalone cache.
4. Every scene that the generated `main.cpp` names has a generated source. A never-saved scene
   ships as the editor holds it, as the prebuilt (cooked) path already does, with a visible
   warning.
5. The exported capacities (`MAX_*`) count every prefab the build registers, including prefabs
   that only scripts spawn.
6. When a loading scene is set, it covers the start scene too, not only later loads.
7. Exports made on a Windows host ship D3D11 shaders as bytecode, so games skip `D3DCompile`.
8. A failed build says why: the first error goes in the failure message, the list goes in the
   Build dialog (each line clickable), and the errors are repeated at the end of the log. The
   Console counts and filters build error lines as errors. The same applies to the Play-time C++
   script build.
9. Every web export has an `index.html` and a favicon. `jasperengine.py` names the page to open.
10. Project and engine paths with spaces, `@` or glob characters (`[`, `]`, `*`, `?`) work in web
    preload arguments and in every CMake glob of the exported build.
11. Generated scene code keeps `hasTextureRect`, as the cooked path does.
12. Every mode's result carries `warnings[{code, message}]`. The Build dialog, the CLI and MCP
    show them.

## 2. Non-goals

- **Doriax's stale-key hashes and warning (7401e66e7).** Rejected. Jasper derives the keys from
  content at every export: `loadAndSaveAllScenes` saves every scene, and `collectSavedShaderKeys`
  runs `ShaderVariantCollector`. A missing variant in a game draws with the nearest shipped one and
  says so (`ShaderPool.cpp:1078-1099`), and does not exit. Goal 3 closes the remaining gap: objects
  created at runtime.
- **DXBC on Linux or macOS hosts.** There is no permissively licensed DXBC compiler: vkd3d-shader
  and Wine's d3dcompiler are LGPL, and DXC emits DXIL (SM6), which D3D11 cannot load. Those hosts
  keep shipping HLSL source, with a note in the result.
- **The editor's own handling of old exports inside the project.** The asset database indexing
  them and the Play-time C++ build compiling them belong to `project-window` and the asset
  pipeline. This feature stops new ones from being made and leaves old ones out of builds.
- **Stale files of a source export written into a non-empty folder with Overwrite.** They stay, as
  documented today.
- **The key of the shared compile tree.** It is computed in `prepareCacheTarget`, before scenes are
  saved (`Exporter.cpp:1318-1345`). That is existing behaviour, and Ninja recompiles when the
  definitions differ.
- **The editor's Play covering its start scene with the loading scene.** Play starts the scene
  being edited, whose objects are already loaded, so there is nothing to cover. Later loads in Play
  are covered as before.
- **Visual Studio solutions in workspaces.** The SDK has none (`engine/workspaces` holds
  `androidstudio` and `xcode` only).

## 3. Users and stories

- **A human exporting a game.** A texture in `assets/ui/` cannot be copied because the destination
  holds a folder of that name. The Build dialog says "Build failed: Failed to copy assets/ui/logo.png
  to …/project/assets/ui/logo.png: Is a directory". Today the export reports success and the game
  ships without its logo.
- **A Unity user who builds into `<project>/Builds/`.** Build Settings marks the folder in red:
  "The output folder is inside the project: the editor would import the exported files as project
  assets. Choose a folder outside it, e.g. …/MyGame-build". The Build button stays disabled until
  the folder is changed. A `Builds/` folder left over from an earlier CLI export is left out of the
  next build, and the dialog lists `W-EXPORT-FOLDER-SKIPPED: Builds/ (an earlier export) was left
  out`.
- **A human who deleted a forked shader.** The next export used to fail with "One or more shaders
  failed to build or save". Now the scene's next save (or the export, which saves it) drops the
  fork's keys, and the build succeeds.
- **A human whose script creates enemies with a material variant no authored object uses.** After
  one Play, Stop saves that variant into the scene, and the exported game draws the enemies with
  the right shader instead of a fallback.
- **A human with a C++ script error.** The Build dialog shows "Build failed with 2 errors. First:
  scripts/Player.cpp:12: 'jump' was not declared in this scope", lists both errors as links that
  open the file at the line, and has "Copy Errors". The Console's Error counter shows 2.
- **A web user uploading to itch.io.** The upload needs `index.html` at the root, which every web
  export now has, with the project's icon (or the JasperEngine logo) as favicon.
- **A Windows user whose name has a space (`C:\Users\Ana Maria\…`).** The web Compile build works:
  the preload arguments are quoted.
- **An agent.** `build_project` fails with `errors[]` and a message naming the first error. It
  succeeds with `warnings[]`, for example `W-SCENE-NOT-SAVED` or `W-SHADER-FORK-MISSING`, and
  `notes[]` (D3D11 shaders as source on a Linux host). An `output_dir` inside the project is
  refused with the same text the editor uses.

## 4. Findings

All findings come from reading the code at `b4f7816ea` (inferred). None was run.

| # | What is wrong or missing | Jasper evidence | Doriax |
|---|---|---|---|
| F1 | Copy and write failures are ignored, so the export reports success with files missing | `editor/Exporter.cpp:2565` (generated), `:2619`/`:2639` (`writeIfChanged` of main.cpp and scene_scripts.cpp unchecked), `:2886` (assets), `:3049` (Lua), `:3096`/`:3117`/`:3188` (C++ scripts), and about 30 more unchecked `writeIfChanged` calls in the Android and Apple settings (`:4129-4349`, `:4446-4780`, `:5476-5552`). `ExporterPrebuilt.cpp:647` drops the icon on error and `:1078`, `:1091` leave notices and the favicon unchecked. Iterators: `copyAssets`/`copyLua` use `++it`, which can throw (`:2769`, `:3026`), and `copyGenerated` ends its loop silently on an iterator error (`:2554`). Checked today: `copyCustomShaderSources` (`:2980-2984`) and the FBX import copy (`:2844-2848`) | 485881045, d8a4feb25 |
| F2 | An export folder inside the project ships inside the next export: its assets, its `.vert`/`.frag`/`.glsl` (`copyCustomShaderSources` walks the project root), and every engine header (`copyCppScripts` copies all project headers, `:3126-3150`) | Walkers skip only `Project::isNonAssetTopLevelDir` (`editor/Project.cpp:5214`). `checkTargetDir` refuses nothing (`Exporter.cpp:1406-1429`), and the build mode bypasses it (`prepareCacheTarget`, `:1431`). Only MCP refuses output inside the project (`editor/mcp/tools/BuildTools.cpp:924-930`). Build Settings only warns about a non-empty folder (`ExportWindow.cpp:1050-1052`), and the CLI passes the folder through (`CommandLine.cpp:675`) | de8dc7340, 14670673b |
| F3 | IDE output in the SDK's workspaces ships, and a re-export deletes the user's IDE state in an exported workspace. `copyTreeIfChanged` copies everything and prunes everything the source lacks: `.gradle`, `.idea`, `local.properties` (sdk.dir), `app/build`, `xcuserdata` | `Exporter.cpp:3326-3381`. `CopyEditorEngine.cmake:110-115` excludes only `.cxx`, `.gradle`, `build` and `.git` from the SDK | de8dc7340 |
| F4 | Saved keys of a deleted fork stay forever, and every later export fails building them | Kept on save: `Project.cpp:3967`, `:7213-7215`, `Stream.cpp:94-130`. Throw: `ShaderBuilder.cpp:312-330`. Failure: `Exporter.cpp:5901-5903`, `:5944-5947` | afc7a1072, b8fa1c832 |
| F5 | The variants of objects scripts create during Play never reach the saved keys | `collectSceneShaderKeys` walks authored entities, foliage and spline chunks only (`Project.cpp:3704-3773`). Stop collects in `finalizeStop` (`:6218-6240`) after the scripts are released (`:10390-10412`). Scenes replaced during Play release their scripts in `retireReplacedRuntimeScenes` (`:9880-9897`) and are never collected with their runtime objects | a3ba6c5c8 |
| F6 | Four copies of the "keys of the build" computation | `Exporter::resolveShaderKeys` (`Exporter.cpp:1183`), `BuildTools.cpp:280-289` `sceneShaderKeys`, `ExportWindow.cpp:129-140` `refreshShaderSelection`, `Project.cpp:4088-4097` `collectStandaloneShaderKeys` | n/a |
| F7 | A never-saved scene has no generated source, but `main.cpp` names it. The source export ships a project that does not link. The cooked path ships it from memory | `clearGenerated` (`Exporter.cpp:2192-2212`), the save loop skips an empty filepath (`:2273`), `configureGenerator` lists every scene (`Project.cpp:7421-7446`), and `updateGeneratedSources` returns during an export (`:7467`). Cooked: `SceneCooker.cpp:289-298` | f2427dcc5 |
| F8 | A prefab placed in no scene does not count toward the capacities. The prebuilt decision and the Compile definitions use scenes only | `Exporter.cpp:3982-4012`, `ExporterPrebuilt.cpp:162`. Model-backed counts need `addModelFile` because a CLI export loads no model (`Project.cpp:3622-3636`) | b1df76163 |
| F9 | The loading scene never covers the start scene. The composite present needs a main scene (Vulkan stacked frames) | `SceneManager.cpp:221-241` goes straight to `startLoading`, and the cover exists only in `applyPendingLoad` (`:247-269`). `Engine.cpp:1325-1333` | 8b5625da4 |
| F10 | Windows exports ship HLSL source, so each game compiles every shader at runtime | `buildAndSaveShaders` has no compile step (`Exporter.cpp:5868-5871`). The runtime already prefers bytecode (`SokolShader.cpp:155-158`), and the serializer stores it (`ShaderDataSerializer.cpp:114-120`, `:457-475`) | bef7f73af |
| F11 | A failed build says only "Build failed. See the Console for details.", shown as "Build failed: Build failed. See…". The Console files build error lines under Log | `Exporter.cpp:1618`, `:1686`, `ExportWindow.cpp:1597`, `Generator.cpp:3378`, `ConsoleWindow.cpp:206-211`. Only MCP extracts errors (`BuildTools.cpp:402-470`, `:698-705`) | afadc81ec |
| F12 | Compiled web exports have no `index.html` (`<app>.html` instead) and no favicon unless one is configured. The prebuilt web page uses the project icon. `jasperengine.py` never names the page. MCP's `index_html` branch is unreachable | `Exporter.cpp:2066-2088`, `:3792`, `ExporterPrebuilt.cpp:1083-1092`, `engine/jasperengine.py:199-212`, `BuildTools.cpp:683-684` (mode web parses to Player) | e91a7d40e, e585159ab, 559e0e8e5 |
| F13 | Unescaped and unquoted paths: `--preload-file ${PROJECT_ROOT}/assets@/` breaks on `@` and spaces, and `--pre-js` on spaces. Globs over `PROJECT_ROOT` and the SDK copy break on `[ ] * ?`. The build tree is in the user cache (`C:\Users\<name>\…`), so a space in a Windows user name reaches every web Compile build | `engine/CMakeLists.txt:354`, `:361`, `:420`, `:2165`, `:2188`, `:2258`, `:2421`; GLOBs in `engine/libs/{thorvg,rmlui,lunasvg,astcenc,zstd,msdfgen,lexbor,meshoptimizer}/CMakeLists.txt` and `engine/libs/shadercompiler/embed_shaderlib.cmake` | 2874de80f |
| F14 | Generated code omits `hasTextureRect`. With async loading, a sprite whose buffers load before its texture may pick a variant without `Txr` | `editor/Factory.cpp:1256` (cooked keeps it: `Stream.cpp:1728`) | 33172858c |
| F15 | Warnings of non-Player exports are invisible. `ExportResult` is reset only in `runPlayerBuild` (`ExporterPrebuilt.cpp:218`), and the Build dialog shows warnings only for Player (`ExportWindow.cpp:1636`) | | n/a |

Dossier items not taken: 2d4857eb0 is already ported (`Project.cpp:6943-6948`), d8a4feb25 does not
apply (its two behaviours are kept in 6.1), and 7401e66e7 is rejected (Non-goals).

## 5. Product decisions

1. **A failed copy or write fails the export.** The error names the project-relative source, the
   destination and the OS reason. A partial game is worse than no game: Unity's build fails on any
   unreadable asset too.
2. **An unreadable folder is skipped with a warning (`W-UNREADABLE-FOLDER`), not a failure.**
   Doriax kept `skip_permission_denied` after its failing variant broke exports (d8a4feb25). The
   warning makes the skip visible. Every other directory error fails the export.
3. **Output inside the project is refused everywhere, except under a hidden folder or the
   top-level `build/`.** Hidden folders include `.jasperengine/builds/…`, MCP's default. The
   assets root is the project root by default, so an export inside it would be imported as
   duplicate assets (prefabs and scenes without their metas get new GUIDs). Unity keeps `Builds/`
   beside `Assets/`, and Jasper has no such separation. The rule and its message are one function,
   which the three front ends share.
4. **Earlier exports inside the project are left out and named.** They are recognised by markers
   that only exports have (6.3). The warning code is `W-EXPORT-FOLDER-SKIPPED`, so a false match is
   visible.
5. **IDE state in an exported workspace belongs to the user.** IDE output is never copied from the
   SDK and never pruned from the destination. Pruning `local.properties` broke Gradle until
   Android Studio rewrote it. This differs from Doriax, which prunes it.
6. **Dead fork keys are dropped silently at save, and with a warning at export.** A fork "is dead"
   when its sources do not resolve with the same resolver the shader build uses. A fork that the
   content still references is S04's missing-asset error (`MissingAssetTracker.cpp:70` covers
   shader references), so it is never silently dropped. Only history keys go.
7. **Stop records the variants of everything that drew during Play:** objects created by scripts,
   prefab spawns, and objects in scenes a load replaced (collected when the scene is retired). The
   `DontDestroyOnLoad` scene's variants go to the played scene. An export unions every scene's
   keys, so attribution only decides which file stores them.
8. **A never-saved scene ships as the editor holds it, with `W-SCENE-NOT-SAVED`.** This matches
   the cooked path. Unity would leave an untitled scene out of the build, but Jasper's build is
   every project scene and the export already saves the others. Build Settings shows the warning
   before the build with a **Save…** button. A never-saved start scene is used for that build but
   is not stored as the project's start scene, because its id has no file.
9. **Capacities count every registered prefab, loaded or not.** They are read from the file when
   not loaded, as the shader collection does.
10. **The loading scene covers the start scene** in every exported game, Compile and prebuilt, when
    a loading scene is set and is not part of the start stack. The order change is documented: the
    loading scene's scripts start on frame 1, and the start scene's scripts start when it loads.
11. **D3D11 bytecode only on Windows hosts, and not in the Json format.** A bytecode compile
    failure fails the export, since the game would fail the same way at runtime. Other hosts keep
    source with a note.
12. **Build errors: the first one in the message, up to 20 unique ones in the dialog and at the log
    end, and the count always given.** MSBuild repeats each error with ` [x.vcxproj]`, and
    duplicates are dropped. Matching and parsing are one implementation for the Console, the
    exporter, the Play build and MCP.
13. **The web page is `index.html`, as Unity, itch.io and the prebuilt web player expect.**
    `<app>.html` is no longer written, and a stale one left by an earlier export is removed from
    the destination. The favicon falls back from Web > Favicon to the Player icon (PNG) to the
    JasperEngine logo. Unity's default WebGL template carries the Unity favicon too.
14. **`W-…` codes for warnings, `notes` for information.** This follows S30's existing
    `ExportResult` fields.

## 6. Design

### 6.1 Shared export file layer: `editor/export/ExportFiles.{h,cpp}` (new)

Namespace `jasperengine::editor::exportfiles`. Exporter.cpp becomes its main caller. The layer has
no Project dependency, so it can be unit-tested.

```cpp
// Walks `root` depth first, without following directory links. `filter` decides per directory:
// Descend or Skip. A directory that cannot be opened with permission_denied goes to
// result.unreadable and is skipped; any other error stops the walk (result.error names the
// directory). `visit` returning false stops the walk (the visitor set its own error).
// An entry whose path relative to root is empty is skipped (d8a4feb25's rule).
struct WalkResult { bool ok = true; std::string error; std::vector<fs::path> unreadable; };
enum class DirAction { Descend, Skip };
WalkResult walk(const fs::path& root,
                const std::function<DirAction(const fs::directory_entry&, const fs::path& rel)>& filter,
                const std::function<bool(const fs::directory_entry&, const fs::path& rel)>& visit);

// permission_denied: the directory is skipped (reported); any other error stops the walk
bool isSkippableOpenError(const std::error_code& ec);

// copy_file with parent directories created; false with "Failed to copy <from> to <to>: <reason>"
bool copyFile(const fs::path& from, const fs::path& to, std::string& error);
// FileUtils::writeFile, failure as above ("Failed to write <path>: <reason>")
bool writeFile(const fs::path& path, const std::string& content, std::string& error);

// Android Studio / Gradle / Xcode / Finder output (Doriax isIdeOutput, plus .kotlin):
// .gradle .idea .cxx .externalNativeBuild .kotlin build captures local.properties xcuserdata
// DerivedData .DS_Store *.iml
bool isIdeOutput(const fs::path& path);

// A folder an export made: jasperengine.py (source export), content-manifest.json (every Player
// build), <app>.wasm or jasperengine-player-st.wasm (web), <app> / <app>.exe (a regular file),
// <app>.app (macOS bundle). `appName` is Factory::toIdentifier(project name).
bool isExportFolder(const fs::path& dir, const std::string& appName);

// The mirror copy of copyTreeIfChanged (unchanged files keep their mtime; what src lacks is
// pruned), with `keepIdeOutput`: IDE output is neither copied nor pruned.
bool mirrorTree(const fs::path& src, const fs::path& dst, bool keepIdeOutput, std::string& error);
```

`walk` uses an explicit stack of `fs::directory_iterator`, each opened and advanced with an
`error_code`. Paths reach strings only through `FileUtils::pathToUtf8` and `pathToGenericUtf8`, never
`.string()`, as `unicode-text` requires. `Exporter::copyTreeIfChanged` and `copyTree`
(`Exporter.cpp:3321-3381`) are replaced by `mirrorTree`.

### 6.2 Copies and writes fail the export (F1)

- New private members in `Exporter`: `bool copyOrFail(const fs::path& from, const fs::path& to)`
  and `bool writeOrFail(const fs::path& path, const std::string& content)`. Each calls the
  `exportfiles` function and, on failure, `setError(error)` with the project-relative form of
  `from` when it is inside the project.
- Every site in Section 4 F1 uses them. The `writeIfChanged` calls in `writeAndroidProjectSettings`,
  `writeAppleProjectSettings`, `writeNativeAppleFiles`, `writeExportSettingsScript` and
  `copyEngine` (`:3481-3498`) go through `writeOrFail`, and each caller returns false. The removals
  of stale export files (`copyAssets` cooked sources `:2808`, the lua duplicate `:2851`) check
  their `error_code`.
- `copyAssets`, `copyLua`, `copyCustomShaderSources`, the header walk and the script-root walk of
  `copyCppScripts`, and `copyGenerated` use `exportfiles::walk` with a filter that applies their
  existing skips:
  - `isNonAssetTopLevelDir` on the first component, with `disable_recursion_pending` replaced by
    `Skip`. `copyCustomShaderSources` now skips non-asset folders instead of descending into them.
  - native plugin roots;
  - `isExportFolder` (6.3).
  Each walk's `unreadable` becomes `warn("W-UNREADABLE-FOLDER", "<rel> could not be read and was
  left out")`.
- `copyPngWithoutEditorChunks` returning false falls back to `copyOrFail`. Today that fallback is
  an unchecked `copy_file`.
- `copyGenerated`: the start-scene substitution in `main.cpp` (`:2596-2616`) fails the export when
  `config.startSceneId` names a scene and no `SceneManager::loadScene("` call is found. Today the
  default start ships silently.
- `ExporterPrebuilt.cpp`: the icon copy (`:647`), `THIRD-PARTY-NOTICES.txt` (`:1078`) and the
  favicon (`:1091`) use the helpers. A missing notice is a licence breach, not a detail.
- `collectWebArtifacts` removes the destination's `<app>.data` when the build made none (Doriax
  2874de80f), so a stale bundle never ships next to a new page.

### 6.3 Output folder and earlier exports (F2)

- `static std::string Exporter::outputFolderRefusal(const Project* project, const fs::path& dir)`
  is public. It returns "" or the refusal. It resolves `dir`, the project root, the assets dir and
  the Lua dir with `weakly_canonical`. `dir` is refused when it equals or lies below any of them,
  unless the path from the project root to `dir` has a hidden component (`.`-named) or starts
  with the top-level `build`. Message: `The output folder <dir> is inside the project: the editor
  would import the exported files as project assets. Choose a folder outside it, e.g. <parent>/<app>-build.`
  It keeps the phrase "inside the project" that `test_refusals` checks.
- `runExport` calls it first for `config.targetDir` (source) or `config.destinationDir` (build
  modes), and `runPlayerBuild` calls it for `config.destinationDir`, before any step.
  `checkTargetDir` and `prepareCacheTarget` stay as they are.
- MCP `build_project` replaces its inline check (`BuildTools.cpp:924-930`) with this function.
  `outputDirRefusal` (the trust check) stays in front of it.
- Build Settings (`ExportWindow.cpp` validation block, `:1039-1065`) computes the refusal when
  `m_targetDir` changes (cached, not per frame) and shows it in red. **Build** and **Export** are
  disabled while it is set. The existing "not empty" warning stays.
- The CLI (`CommandLine.cpp`) gets the refusal from the exporter, prints it, and exits 1.
- `bool Exporter::isExportFolder(const fs::directory_entry&) const` is true for `config.targetDir`,
  `config.destinationDir`, `getBuildCacheDir()` (via `fs::equivalent`), or
  `exportfiles::isExportFolder(dir, getAppName())`. A skipped folder is warned once per export:
  `warn("W-EXPORT-FOLDER-SKIPPED", "<rel> looks like an earlier export (<marker>) and was left out;
  move it out of the project")`.
- The same marker test (static form, without the config folders) is used by
  `Exporter::chooseBuildMethod`'s script scan (`ExporterPrebuilt.cpp:128-141`), so an engine copy
  inside an old export does not force Compile, and by `webTreePathRefusal`.

### 6.4 IDE output (F3)

- `copyEngine`'s `copyDir("workspaces")` uses `mirrorTree(src, dst, /*keepIdeOutput=*/true, …)`.
  The other SDK folders use `keepIdeOutput=false`, which keeps today's behaviour.
- `CopyEditorEngine.cmake:110-115` adds `PATTERN` excludes for `.idea`, `.kotlin`,
  `.externalNativeBuild`, `captures`, `local.properties`, `xcuserdata`, `DerivedData`,
  `.DS_Store` and `REGEX "\\.iml$"`, so an installed SDK is clean as well.

### 6.5 Shader keys (F4, F5, F6)

**6.5.1 One build key set.**
`void Project::collectBuildShaderKeys(std::set<ShaderKey>& keys, std::vector<std::string>* droppedForks = nullptr) const`
unions, for every scene: `sceneProject.shaderKeys`, `collectSceneShaderKeys` and
`collectRequiredShaderKeys`, each normalised (`ShaderPool::normalizeKey`). It then drops dead forks
(6.5.2). `Exporter::resolveShaderKeys` (the non-override branch), `BuildTools::sceneShaderKeys`,
`ExportWindow::refreshShaderSelection` and `Project::collectStandaloneShaderKeys` all call it.
`resolveShaderKeys` also filters `config.shaderAdditions` and the warm-up list with the same
dead-fork test. Every dropped fork is warned once:
`W-SHADER-FORK-MISSING: variants of shaders/foo were left out: its files are gone`.
An explicit `--shader` list (`shaderKeysOverride`) is the user's word and is not filtered: a dead
fork there fails as today, but with the better message of 6.10.

**6.5.2 Dead forks.**
`static bool ShaderBuilder::customShaderResolves(Project*, ShaderKey key, std::string* error)`
wraps the file-local `resolveCustomSources` (`ShaderBuilder.cpp:62`). It is true for keys with
customId 0, for library ids (`ShaderPool::isLibraryCustomShader`), and for
`ShaderReference::isLibrary` and `ShaderReference::isBuiltin` names. Otherwise it is true when the
sources resolve. This is the same resolver the export build uses, so split `vert|frag` names,
assets-relative files and UI-effect `.frag` forks are judged exactly as the build judges them
(b8fa1c832's lesson). `saveSceneFile` (`Project.cpp:7213-7215`) applies it after
`collectSavedShaderKeys`: the key set saved to the file loses dead-fork keys. This takes
`resolveCustomSources`' cache lock once per distinct fork. Failures are not cached, so a fork
restored on disk is live again at once.

**6.5.3 Play-created objects.**
- `collectSceneShaderKeys(const SceneProject*, std::set<ShaderKey>&, bool includeRuntimeEntities = false)`:
  with the flag, the entity list is the authored list plus every entity of
  `scene->getEntityList()` at or above `EntityManager::firstUserEntity()` that is not already
  listed (Doriax a3ba6c5c8). System entities (Scene view gizmos) stay out.
- `PlaySession` gains `std::map<uint32_t, std::set<ShaderKey>> playShaderKeys`, keyed by source
  scene id and guarded by `playSessionMutex`.
- `retireReplacedRuntimeScenes` (`Project.cpp:9880`) collects with the flag **before**
  `conector.cleanup` / `cleanupLuaScripts`, into `playShaderKeys[entry.sourceSceneId]`.
- The Stop task (`Project.cpp:10350`) collects every live runtime scene with the flag right after
  copying `runtimeScenes`. This is before `ScriptRuntime::quitAll` (moved ahead of it: `quitAll`
  runs `onApplicationQuit`, which may destroy objects) and before every cleanup. The persistent
  scene's keys go to the session's main scene id.
- `finalizeStop(mainSceneProject, runtimeScenes, playShaderKeys)` merges the map instead of
  collecting (`:6218-6240`). The existing modified, save-on-stop and `saveSceneShaderKeys` logic
  is unchanged.

### 6.6 Never-saved scenes (F7)

- In `loadAndSaveAllScenes`, after the save loop: for each scene with an empty `filepath` and a
  loaded `scene`, call `project->writeSceneSource(&sceneProject)` (public; it writes into the
  generated folder the export just cleared) and `warn("W-SCENE-NOT-SAVED", "Scene '<name>' has
  never been saved; it ships as it is in the editor. Save it to keep it.")`. A scene with neither
  file nor content (not loaded, no file) fails the export with `Scene '<name>' has never been saved
  and is not open: save it or remove it from the project`.
- `project->configureGenerator()` then runs once on the main thread, so `main.cpp` matches what was
  written.
- **Class check.** After that, still on the main thread, every scene of `project->getScenes()`
  must satisfy `generator.hasSceneSource(id, name, internalPath)` (exposed as
  `Project::hasSceneSource(const SceneProject&)`). A missing source fails the export naming the
  scene, before anything is copied.
- Build Settings lists the never-saved scenes under the validation block, in orange: "Scene
  'Untitled' has never been saved: the build ships it as it is now". Each has a **Save…** button
  that opens the editor's Save As for that scene (`scene-integrity` owns that dialog). The start
  scene combo accepts a never-saved scene: `exportConfig.startSceneId` is set, and
  `setStartSceneId` is called only for a saved one (today both are skipped,
  `ExportWindow.cpp:1395-1400`).

### 6.7 Capacities of every prefab (F8)

- Refactor `Project::calculateSceneMaxValues` (`Project.cpp:3579-3702`) into
  `calculateRegistryMaxValues(EntityRegistry*, const std::function<bool(Entity)>& owned, SceneMaxValues&)`.
  The scene form keeps its `own` filter and its spline rebuild.
- `void Project::calculatePrefabMaxValues(SceneMaxValues& out) const` iterates
  `collectAllPrefabs()`. It uses the loaded prefab's `registry`/`registryEntities`, or loads the
  file into a temporary `Prefab::loadRegistry`, the same way `collectRequiredShaderKeys` does
  (`:3990-4022`). Both functions use one shared helper, `forEachBuildPrefabContent(fn)`, so the two
  do not drift. Model files count through `addModelFile` (glTF external buffers), so unloaded
  prefabs work in a CLI export.
- Results go into `Project::buildPrefabMaxValues` (`SceneMaxValues` plus a `std::mutex`, written on
  the main thread). They are refreshed in `loadAndSaveAllScenes`, on prefab save
  (`savePrefabToDisk`), and by Build Settings and `get_build_info` before they call
  `chooseBuildMethod`.
- `Exporter::sceneMaxValuesOf` merges `project->getBuildPrefabMaxValues()`, read under the mutex.
  It is the single source for the Compile definitions, the shared-tree key and the prebuilt
  decision.
- If `mesh-memory` removes `MAX_SUBMESHES` (dynamic submeshes), that macro leaves the list, and
  this design holds for the rest. If it stays, `addModelFile` also counts glTF primitives per
  MeshComponent (`countGltfExternalBuffers` gains a primitives count in the same cached JSON pass),
  so an unloaded model-backed prefab or scene counts its submeshes.

### 6.8 Loading scene covers the start scene (F9, engine)

- `SceneManager::loadScene` (`SceneManager.cpp:221`): `const bool coverStart = loadingSceneId != 0
  && canShowLoadingScene(id);` joins the pending condition, giving `isFrameRunning() || getMainScene()
  || factoryRunning || coverStart`. Jasper's `factoryRunning` term is kept. A second `loadScene`
  before the first frame replaces the pending one, with the existing warning.
- `applyPendingLoad` is unchanged. On the first frame it adds the loading scene as a layer (there
  is no main scene yet), waits for `loadingSceneReady` and `loadingDelay`, then `startLoading`.
- `Engine::endCompositeFramebuffer` (`Engine.cpp:1325-1333`) presents through `mainScene`, or,
  when there is none, through the first scene of `scenes` (under `sceneListMutex`). Without this,
  a Vulkan frame with a stacked overlay (debug overlay, screenshot run) would present nothing.
- **Audit (implementation task).** Check every `Engine::getMainScene()` reader outside Engine.cpp
  (10 sites in `engine/core`) for null during the cover frames, and fix any that assume a main
  scene. The test in Section 10 runs cover frames and fails on any logged error.
- With no loading scene set, the path is byte-for-byte today's. The editor's Play is unchanged:
  it calls `SceneManager::setCurrentScene`, not `loadScene`, for its start (`Project.cpp:9724`).
- Docs: `docs/mcp/guide/lua-api.md` and the scenes and loading documentation explain that the
  loading scene now covers the start, and the script order (loading scene first).

### 6.9 D3D11 bytecode (F10)

- `static void ShaderBuilder::compileHlslBytecode(ShaderData&)` follows Doriax 3fb49fbba's
  `ShaderBuilder.cpp:1092`, under `#ifdef _WIN32`. It loads `d3dcompiler_47.dll` once (static
  lambda) and gets `D3DCompile`. For each stage it compiles with entry `main`, target
  `vs_5_0`/`ps_5_0` and flags `D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_OPTIMIZATION_LEVEL3`,
  the same as sokol's release compile (`sokol_gfx.h:13638-13642`). It stores the blob in
  `stage.bytecode`, with allocation and ownership as the SPIR-V bytecode path of `ShaderData`
  uses, and clears `stage.source`. On failure it throws with D3DCompile's messages. Without the
  DLL it returns false and source stays. Signature: `static bool compileHlslBytecode(ShaderData&,
  std::string& error)`, returning false only when the DLL is missing.
- `Exporter::buildAndSaveShaders` (`:5867`): for `backend == ShaderBackend::D3D11 &&
  config.shaderOutputFormat != ShaderOutputFormat::Json`, it calls the function on Windows. On
  other hosts, or without the DLL, it adds one note: "D3D11 shaders ship as HLSL source: the game
  compiles each when first used. Build on Windows to ship bytecode."
- `ShaderDataSerializer.cpp:114` changes its comment to "SPIR-V (Vulkan) and DXBC (D3D11 exports
  built on Windows)". The format is unchanged: the bytecode field already exists, and old `.sdat`
  files have size 0 there.
- Runtime: no change. `SokolShader.cpp:155-158` and `SokolAsyncCreate.cpp:86-90` take bytecode as
  they are. This Windows-only code cannot be built here. The implementer keeps it compiling by
  following Doriax's version and the existing `_WIN32` patterns of `CommandRunner.cpp`, and says
  so in the report.

### 6.10 Build errors (F11)

- **`editor/util/BuildDiagnostics.{h,cpp}` (new).** `struct BuildDiagnostic {file, line, column,
  severity, code, message, text}` (moved from `BuildTools.cpp:402-470` with `parseDiagnostic` and
  `stripAnsi`), plus:
  - `static LogLevel BuildDiagnostics::lineLevel(const std::string&)`. `Out::buildLineLevel`
    (`Out.cpp:207-219`) forwards to it. It adds `": error MSB"`, `"error LNK"` and
    `": fatal error C"` to today's markers.
  - `class BuildErrorLog { void add(const std::string& line); size_t errorCount() const; const
    std::vector<BuildDiagnostic>& errors() const; std::string firstError() const; }` keeps up to
    20 unique errors, after stripping MSBuild's trailing ` [*.vcxproj]` and leading whitespace, and
    counts all of them. It also keeps the last non-empty line, for a failure with no recognised
    error.
- **Exporter.** `configureBuild` and `runBuild` feed every line to a `BuildErrorLog`. On failure,
  and when not cancelled:
  1. log `Build errors (N):` with each kept error as its own `Out::error` line (each with its
     location, so the Console links it);
  2. `setError("Build failed with N error(s). First: <file>:<line>: <message>")`. For configure
     the wording is "Build configuration failed with …". With no recognised error the message is
     `Build failed; the last output was: <line>`.
  `setError`'s own `Out::error` stays the last line.
- `ExportProgress` gains `std::vector<BuildDiagnostic> errors` and `size_t errorCount`, copied
  under `progressMutex`. `buildAndSaveShaders` also records each failing shader as a
  `BuildDiagnostic` (`file` = the fork's entry point when custom, the message = the builder's
  exception), and its final message names the first: `<n> shader(s) failed to build. First: <name>
  (<lang>): <error>`.
- **Build dialog** (`ExportWindow::drawProgress`):
  - Failed: the message, then "Errors (N)" in a scrolling child at most 8 lines high. Each row
    shows `file:line: message`. A row with a location is a link: a click resolves it with
    `resolveSourceLocation` and the Console's roots (build-tree copies map back to project files)
    and opens it with `ExternalCodeEditor::open`, as `ConsoleWindow::openLocation` does (extract
    a shared `openSourceLocation(const SourceLocation&, const SourceRoots&, bool external)` into
    `editor/util/SourceLocation`). Buttons: **Copy Errors** (all kept errors to the clipboard) and
    **Close**.
  - Succeeded: when `result.warnings` is not empty, "Completed with N warning(s)" in orange and the
    list, for every mode (today Player only, `:1636`).
- **Console.** `ConsoleWindow::severityOf(LogType, LogLevel)` maps a Build line with level Error to
  `SEVERITY_ERROR_MESSAGE` and with level Warn to `SEVERITY_WARNING`. The hammer icon stays, the
  text takes the severity colour, and the counters and filters include these lines. MCP
  `read_console` keeps type `build` (no MCP change).
- **Play-time C++ build** (`Generator.cpp:3355-3378`). `buildProject` and `configureCMake` feed a
  `BuildErrorLog`. On failure the log ends with `C++ scripts failed to build with N error(s):` and
  the errors as `Out::error` lines. `ScriptTools.cpp:1522` and `PlayTools.cpp:1442` use the shared
  parser.
- **MCP.** `BuildTools.cpp` uses `BuildDiagnostics`, and `r.diagnostics` comes from the same
  parser. `finishRun` takes `progress.errorMessage` as the job message and appends its own
  "compiler error(s), first:" only when `progress.errors` is empty (non-export failures). The
  result shape is unchanged: `errors[]`, `warnings[]`, `error_count`.
- **CLI.** `jasperengine-editor build` prints the kept errors to stderr before its final failure
  line, and on success prints `Completed with N warning(s):` and the list.

### 6.11 Web page, favicon, `jasperengine.py` (F12)

- `apply.cmake` (`Exporter.cpp:3792`): after writing `${output}/${app}.export.html`, it writes
  `${output}/index.html` with the same content unless `string(TOLOWER "${app}")` is `index`. Then
  the linker's own `index.html` is the raw output, and `index.export.html` is the page.
- `collectWebArtifacts` copies `<app>.export.html` to `<destination>/index.html`, plus `<app>.js`
  and `<app>.wasm`. It removes a stale `<destination>/<app>.html`. `collectWebThreadsArtifacts`
  (`:2168`) patches `index.html`.
- Favicon: `bool Exporter::writeWebFavicon(const fs::path& target)` takes `web.favicon` (a read
  failure fails the export as today), else `project->getWindowIcon()` when it is a `.png` file,
  else the bytes of `jasperengine_logo_png` (`editor/resources/icons/jasperengine-logo_png.h`). It
  is used by `writeExportSettingsScript` (`export-settings/favicon.png` is then always present, so
  apply.cmake always links it) and by the prebuilt `finalizeWeb` (`ExporterPrebuilt.cpp:1083-1092`),
  where `composeWebPage(…, favicon=true…)` always gets true.
- `engine/jasperengine.py`: after `cmake --build` for `web`, it finds `*.export.html` in
  `build_dir` (glob-escaped) and takes the newest by mtime (Doriax e585159ab). It prints `Web build
  ready: serve <build_dir> over HTTP and open index.html`, or the export page's name when that is
  `index.export.html`.
- MCP: the unreachable `ExportMode::Web` branch in `finishRun` (`BuildTools.cpp:682-684`) is
  removed. A Player web result already reports `executable` = `index.html`.

### 6.12 Paths in the exported CMake build (F13)

- New `engine/cmake/JasperEnginePaths.cmake` (`include_guard(GLOBAL)`):
  `function(jasperengine_glob_escape out path)` does
  `string(REGEX REPLACE "([][*?])" "[\\1]" result "${path}")` (Doriax's escape), so `[` becomes
  `[[]`, `]` becomes `[]]`, `*` becomes `[*]` and `?` becomes `[?]`. The implementer checks with
  the test in Section 10 that CMake's glob treats a `]` first in a class as literal.
- `engine/CMakeLists.txt`: after `file(TO_CMAKE_PATH ${PROJECT_ROOT} PROJECT_ROOT)` it computes
  `PROJECT_ROOT_GLOB`, and near its top `JASPERENGINE_ROOT_GLOB` (the escaped
  `CMAKE_CURRENT_SOURCE_DIR`). Every glob with a path prefix uses them: `:2165`, `:2188`, `:2258`,
  `:2421`.
- Vendored libraries: `meshoptimizer` switches to `aux_source_directory` (Doriax). `thorvg`,
  `rmlui`, `lunasvg`, `astcenc`, `zstd`, `msdfgen` and `lexbor` include `JasperEnginePaths.cmake`
  and glob through `jasperengine_glob_escape(… "${CMAKE_CURRENT_SOURCE_DIR}")`. So does
  `shadercompiler/embed_shaderlib.cmake:19,23`, which escapes its `SHADERLIB_DIR` argument.
- Web link flags: `string(REPLACE "@" "@@" EM_PRELOAD_ROOT "${PROJECT_ROOT}")`, then
  `--preload-file \"${EM_PRELOAD_ROOT}/assets@/\"` (and `lua`), and `--pre-js
  \"${PLATFORM_ROOT}/crash_pre.js\"`. The quotes reach Ninja's and Make's command lines, and
  cmd.exe through emcc.bat, where double quotes group arguments.
- The Windows refusal of `%` and `"` (`webPathRefusal`, `Exporter.cpp:1274-1291`) stays: cmd.exe
  expands `%VAR%` inside quotes.

### 6.13 `hasTextureRect` in generated code (F14)

In `Factory.cpp`, after the `textureRect` line (`:1256`): when `mesh.submeshes[s].hasTextureRect`,
emit `mesh.submeshes[<idx>].hasTextureRect = true;`. This is a one-line parity fix with the cooked
path. It is taken even though the dossier marks its visible effect as uncertain, because the
generated and cooked games must load identical submesh state.

### 6.14 Warnings for every mode (F15)

- `startExport`, `exportProject` and `generateShaders` reset `result = ExportResult()` (with
  `outputDir`) under `progressMutex`. Today only `runPlayerBuild` does.
- `warn(code, message)` is usable from every step. MCP `finishRun` appends `report.warnings` and
  `report.notes` for every mode (`BuildTools.cpp:641-643`, today in the Player branch only).
- Codes added: `W-EXPORT-FOLDER-SKIPPED`, `W-UNREADABLE-FOLDER`, `W-SCENE-NOT-SAVED`,
  `W-SHADER-FORK-MISSING`.

### 6.15 Docs and agent surfaces

- `docs/shipping.md`: output folder rule, `index.html` and favicon fallback, D3D11 bytecode
  (Windows hosts), never-saved scenes, build errors in the dialog, warning codes.
- `docs/mcp/guide/shipping.md`: the refusal, the new warning codes, `errors[]` and the message's
  first error, the D3D11 note.
- `docs/mcp/guide/lua-api.md` (or the scenes guide, wherever `setLoadingScene` is documented):
  the start cover and the script order.
- The agent guide: one line at most. MCP tool descriptions change only if `build_project`'s
  description mentions output inside the project. If it does, regenerate `docs/mcp/TOOLS.md` and
  run `tool_budget.py --check`.
- Undo: no undoable edit is added. The dead-key drop is part of a save, and Stop's key merge is
  existing behaviour. Lua: no new API (`SceneManager.setLoadingScene` and `isLoading` behave as
  documented above).

## 7. Behaviour rules and edge cases

1. A destination path that exists as a directory where a file goes (or the reverse) fails the
   export naming both paths. This is also the deterministic test trigger: tests run as root, so
   permissions cannot be used.
2. A link inside assets to a folder outside is not followed (`walk` does not follow directory
   links). Today `recursive_directory_iterator` without `follow_directory_symlink` already does not
   follow them, so nothing changes.
3. `isExportFolder` checks only directories, once each, before descending. A matched folder is not
   descended. When a marker file is itself an asset (for example `assets/tools/jasperengine.py`),
   its folder is skipped and named in the warning. The user sees it and can rename it.
4. Output folder rule: `<project>/.jasperengine/builds/x` and `<project>/build/web` are allowed.
   `<project>/Builds`, `<project>/assets/out` and `<lua dir>/x` are refused. A symlinked output
   resolving into the project is refused, since `weakly_canonical` resolves it. An output that is
   an ancestor of the project (`dir` = parent of project) is allowed for builds. The walkers skip
   the export's own folder by equivalence.
5. Dead fork keys: a fork with one stage missing (a `vert|frag` whose `.frag` was deleted) is dead.
   A library (`@effects/…`) or `builtin:` key is never dead. A project UI effect `.frag` resolves
   assets-relative first (`ShaderBuilder.cpp:83-96`).
6. Play keys: a Play that fails at startup collects nothing (`startupSucceeded` false). A scene
   loaded and replaced several times accumulates into one set. Keys of a runtime copy with no
   source scene (`NULL_PROJECT_SCENE`) go nowhere, as today.
7. Never-saved scenes: an unsaved scene that is also the start scene gets its source and is the
   start. A scene renamed but never saved keeps its in-memory name in `main.cpp`.
8. The loading cover's start is skipped when the start stack contains the loading scene
   (`canShowLoadingScene`). A script in the loading scene's `init` that calls `loadScene(other)`
   replaces the pending start, with the existing warning.
9. D3D11 bytecode with a source export from Windows (Header format) ships bytecode too, and the
   user's own build then needs no d3dcompiler at runtime. Json output (`generate_shaders --format
   json`) stays source for readability.
10. Build errors: a cancelled build reports "Build cancelled" only, with no error list. Twenty
    unique errors are kept, the count covers all of them, and the dialog says "(showing 20 of 57)".
11. Web: an app named `Index` on case-insensitive filesystems is handled by the lowercase compare.
    A custom HTML shell keeps working: index.html is the shell-wrapped page.
12. The favicon fallback uses only a PNG `getWindowIcon()`. Another format falls through to the
    logo.

## 8. Performance and cost

- **Runtime.** The loading cover adds one boolean check in `loadScene`. With no loading scene set
  it costs nothing, and no frame is spent on the start. With one set, the start is delayed by the
  cover's own `loadingDelay`, which is the point of the feature. `endCompositeFramebuffer`'s
  fallback runs only while there is no main scene. D3D11 bytecode removes one `D3DCompile` per
  shader per run. The gain is not measured here, and the implementer records the first-load time of
  a sample scene before and after on Windows, if one is available (otherwise "not measured"). No
  other runtime change.
- **Export time.** The walkers add up to 7 `stat` calls per directory (markers): O(directories),
  negligible next to file copies. The dead-fork test costs one source resolution per distinct fork,
  cached on success. The D3D11 compile adds about 10-500 ms per HLSL variant on Windows hosts. It
  is serial like the rest of `buildAndSaveShaders`. If a project with 200+ variants exports more
  than 20% slower, the implementer runs the compile in a `jobs::ParallelFor` (D3DCompile is
  thread-safe) and records the numbers.
- **Editor.** Stop walks every entity of each runtime scene once (O(entities), Play only). Build
  Settings computes the output refusal on folder change, and the prefab maxima on open and when a
  prefab is saved, not per frame. The Console severity change is per line at insert.

## 9. Compatibility

- **No project or scene format change, no version bump.** Saved `shaderKeys` keep their format.
  A scene with dead-fork keys loses them at its next save, a one-time diff that the release notes
  explain.
- `.sdat` and shader headers: the bytecode field exists already, so old readers and files are
  unaffected. Windows exports made by older editors keep working.
- **Export output changes, which `docs/shipping.md` records:**
  - compiled web exports have `index.html` instead of `<app>.html`, and a re-export into the same
    destination removes the old `<app>.html`;
  - `favicon.png` is always present.
- Old export folders inside a project are left out with a warning. A saved Build Settings target
  inside the project is now refused, and the user picks another folder once.
- Prebuilt players: no player rebuild is needed for anything except the loading cover, an engine
  change that reaches prebuilt games when their runtimes are rebuilt. `docs/shipping.md` notes that
  players built before it start without the cover.

## 10. Tests

Red control means run on `main` (or with the fix reverted) and seen failing. "MCP" means
`tools/mcp/tests/test_export_pipeline.py` (new, `CI = True`, created project) unless another file
is named.

| # | Suite and test | Asserts | Red control |
|---|---|---|---|
| T1 | MCP `test_failed_copy_fails_the_export` | Source export with `overwrite: true` into a folder where `project/assets/ui/logo.png`, `project/lua/Game.lua`, `project/scripts/Util.h` (each a parametrised case) and `project/main.cpp` exist as **directories**. The job fails, and its message names the file. | yes: on `main` each succeeds |
| T2 | `jasperengine-editor-tests` `editor/tests/export/ExportFilesTest.cpp`: `ExportFiles.OpenErrorsClassify`, `.WalkFailsWhenTheRootIsNotADirectory`, `.CopyFileFailsOverADirectory`, `.MirrorKeepsIdeOutputInTheDestination`, `.MirrorNeverCopiesIdeOutput`, `.ExportFolderMarkers` | Walker semantics. Tests run as root, which ignores modes, so `walk`'s decision is a public `exportfiles::isSkippableOpenError(std::error_code)` (permission_denied is skipped, everything else fails), unit-tested directly. The walk itself is tested failing when its root is a regular file (ENOTDIR) and when it is missing. Copy errors. The `mirrorTree` IDE rules: dest `.gradle`/`local.properties` survive, source `.idea` is not copied. The markers. | new unit. The mirror tests are red against today's `copyTreeIfChanged` (moved to the new API in a first commit) |
| T3 | MCP `test_earlier_export_inside_project_is_left_out` | The project holds `OldBuild/` containing `jasperengine.py`, `core/Engine.h`, `assets/x.png` and `shaders/a.vert`. A source export to the scratch folder has no `OldBuild` in `project/assets`, `project/scripts` or the shader sources. The result `warnings` has `W-EXPORT-FOLDER-SKIPPED` naming `OldBuild`. `get_build_info`'s method reasons do not list `OldBuild` sources. | yes |
| T4 | MCP `test_output_inside_project_refused` (in `test_build.py::test_refusals` too, unchanged phrase) and CLI via `harness.editor_command()` `build --project P --target P/Builds` | Exit 1 and the refusal text. `P/build/x` and `P/.jasperengine/builds/x` are accepted. | yes for the CLI (on `main` it exports) |
| T5 | `jasperengine-editor-tests` `export/ExportOutputFolderTest.cpp` `ExportOutputFolder.{InsideProjectRefused, HiddenAndBuildAllowed, InsideExternalAssetsDirRefused, SymlinkIntoProjectRefused}` | The rule table of 7.4 | new function |
| T6 | MCP `test_dead_fork_keys_dropped` | A scene file is hand-written with `shaderKeys` naming `customShader: shaders/gone` (no files) and `shaders/a.vert\|shaders/b.frag` (files present, split stage). After `save_scene`, the file keeps the split key and drops `gone`. `build_project mode: source` succeeds with `W-SHADER-FORK-MISSING` absent (the save dropped it). With `gone` added to Build Settings additions (`project.yaml`), the export succeeds with the warning. | yes: on `main` the export fails "One or more shaders failed" |
| T7 | MCP `test_play_created_objects_record_variants` | A Lua script whose `start` creates a GameObject with a Mesh and a material using a feature no authored object uses (vertex colour), and a second scene loaded by `SceneManager.loadScene` whose script does the same with another feature. Play, wait 30 frames, Stop. Both scene files' `shaderKeys` contain the variant (checked through `get_build_info` shader names). | yes |
| T8 | MCP `test_never_saved_scene_ships` | `create_scene` without a path (untitled), then a source export: `project/scene_*_<id>.cpp` exists for it, `main.cpp` names it, the result has `W-SCENE-NOT-SAVED`. `check_xcode_workspace` (from `test_build.py`, moved to a shared helper) passes. | yes: on `main` the source file is missing |
| T9 | MCP `test_script_only_prefab_capacities` | The test writes a glTF in Python with 31 external buffer views on one mesh (and 20 primitives if `MAX_SUBMESHES` still exists after `mesh-memory`), then a prefab using it that no scene places. `get_build_info` for a Linux player reports compile reason `MAX_EXTERNAL_BUFFERS 31`, and a source export's `CMakeLists.txt` defines `MAX_EXTERNAL_BUFFERS=31`. | yes |
| T10 | `jasperengine-editor-tests` `engine/LoadingSceneStartTest.cpp` (fixture pattern of `PersistenceTest.cpp:400-445`): `LoadingSceneStart.CoversTheStartScene`, `.NoLoadingSceneStartsAtOnce`, `.StartStackWithTheLoadingSceneIsNotCovered`, `.NoErrorsWhileCovering` | With a loading scene set, `loadScene(start)` before any frame: after frame 1 the top layer is the loading scene and the main scene is not yet the start; within N frames the main scene is the start and the loading scene is gone. Without one, the start is the main scene before any frame. No error is logged during the cover. | yes: `CoversTheStartScene` fails on `main` |
| T11 | `jasperengine-editor-tests` `shader/HlslBytecodeTest.cpp` `HlslBytecode.ExportShipsBytecodeOnWindows` (body under `#ifdef _WIN32`; the file compiles to no test elsewhere) | A built HLSL5 variant through `compileHlslBytecode` has non-empty bytecode and empty source, and round-trips through `ShaderDataSerializer`. | Windows CI only. Cannot be run here; the report says so |
| T12 | `jasperengine-core-tests` `shader/ShaderDataSerializerTest.cpp` `ShaderDataSerializer.HlslBytecodeRoundTrips` | An HLSL `ShaderData` with bytecode and empty source round-trips (all hosts) | guards the format, not a fix |
| T13 | `jasperengine-editor-tests` `util/BuildDiagnosticsTest.cpp`: `.GccClangMsvcCmakeLinkerLines`, `.MsBuildRepeatsCollapse`, `.KeepsTwentyCountsAll`, `.LastLineWhenNothingMatches` | Parsing and collection (the cases from BuildTools' parser move here) | new unit |
| T14 | MCP `test_build.py::test_desktop_build_and_run` (slow lane, existing compile-error step) | Extended: the failed job's message starts `Build failed with 1 error(s). First: ` and names the C++ file:line. `read_console` shows the `Build errors (1):` block as the last lines before the failure line. | yes (message) |
| T15 | `jasperengine-editor-tests` `ConsoleSeverityTest.cpp` `ConsoleSeverity.BuildErrorLinesCountAsErrors` | `severityOf(Build, Error)` is the error severity, and a `[build]` warning line is a warning | yes |
| T16 | MCP `test_web_page_and_favicon` | Runs the generated `export-settings/apply.cmake` from a source export with `cmake -DAPP_FILE=<tmp>/game.html -DWEB=ON -P` on a stub page. Then `index.html` equals `game.export.html`, `favicon.png` equals the logo bytes (no icon set), and equals the Player icon when one is set. App name `index`: no `index.html` is written over the stub. | yes |
| T17 | Slow lane (`JASPER_SLOW_BUILD=1`), existing prebuilt web player test in `test_build_method.py`, extended | Prebuilt web with no favicon and no icon has `favicon.png` with the logo bytes | yes |
| T18 | MCP `test_export_paths_with_special_characters` | Source export into `<scratch>/My Game @1 [x]/`, then `cmake -S <dir> -B <dir>/build -G Ninja` configure only. `build.ninja` mentions the generated scene source and `scene_scripts.cpp`, and an engine source from `libs/meshoptimizer` and `libs/rmlui`. | yes: on `main` the `[x]` glob finds nothing |
| T19 | `tools/build.sh web-gate` (web image): `tools/web/browser_gate.py` gains `--export-dir-suffix " @1"` used by the gate script | The web Compile build of the gate project in a folder with a space and `@` builds and runs, and the page is `index.html` | yes |
| T20 | `jasperengine-editor-tests` `GeneratedScenesTest.cpp` `GeneratedScenes.SpriteKeepsHasTextureRect` | The generated factory code of a Sprite contains `hasTextureRect = true` | yes |
| T21 | MCP `test_warnings_for_every_mode` | A source export with a never-saved scene returns `warnings[]` with the code. Build Settings is covered by T8's code path (the dialog itself is checked by a screenshot in the report, Section 11) | yes |

Existing suites to keep green: `test_build.py`, `test_build_method.py`,
`test_export_generated_sources.py`, `test_texture_export.py`, `test_html_ui_export.py`,
`BuildMethodTest.cpp`, and `tools/check-changed` per commit.

## 11. Acceptance criteria

1. T1 passes. On `main` (the red control) each case of T1 reports success.
2. No `fs::copy_file`, `fs::copy` or `writeIfChanged` call in `Exporter.cpp` or
   `ExporterPrebuilt.cpp` ignores its result. Checked with
   `grep -nE "copy_file|writeIfChanged\(" editor/Exporter*.cpp`: every hit is inside
   `copyOrFail`, `writeOrFail` or `exportfiles`, or is followed by an `ec` check that fails.
3. T3, T4 and T5 pass. Build Settings shows the refusal and disables Build for a target inside the
   project. A screenshot from an isolated editor goes with the report.
4. T2 passes. An SDK `workspaces/androidstudio/.idea` is not copied, and an exported
   `workspaces/androidstudio/local.properties` survives a re-export.
5. T6 passes. The split-stage key survives a save.
6. T7 passes, including the scene replaced during Play.
7. The Exporter, Build Settings, MCP `get_build_info`/`build_project` and the standalone cache all
   call `Project::collectBuildShaderKeys`, and no other key-union loop remains (grep for
   `collectRequiredShaderKeys(&` outside Project.cpp finds only that function's callers).
8. T8 passes. Build Settings shows the never-saved warning with **Save…** (screenshot).
9. T9 passes.
10. T10 passes. With no loading scene, the start-of-game frame count is unchanged
    (`NoLoadingSceneStartsAtOnce`).
11. On a Windows host, T11 passes. If no Windows host is available, the report says T11 was not
    run and the Windows code was only compiled by reading.
12. T13, T14 and T15 pass. A failed Desktop build in Build Settings lists clickable errors and has
    **Copy Errors** (screenshot).
13. T16 and T17 pass. A compiled web export's destination has `index.html`, `favicon.png` and no
    `<app>.html`.
14. T18 passes. T19 passes in the web-gate image.
15. T20 passes.
16. `docs/shipping.md` and `docs/mcp/guide/shipping.md` describe the rules above.
    `tools/mcp/gen_tool_docs.py --check` and `tool_budget.py --check` pass if tool text changed.
    `tools/check_agent_docs.py` passes if the agent guide changed.
17. `tools/check-changed` is green on the branch, the full MCP suite shows no new failures against
    the `main` baseline, and the `SANITIZE=address,undefined` unit tests pass for the new units.

## 12. Files likely touched

- `editor/Exporter.cpp`, `editor/Exporter.h`, `editor/ExporterPrebuilt.cpp` (hot: `mobile-shipping`,
  `build-toolchain`, `agent-authoring`, `unicode-text` also touch them)
- `editor/export/ExportFiles.{h,cpp}` (new), `editor/util/BuildDiagnostics.{h,cpp}` (new),
  `editor/util/SourceLocation.{h,cpp}` (shared opener)
- `editor/Project.cpp`, `editor/Project.h` (shader keys, Stop, prefab maxima, never-saved
  sources; hot: `scene-integrity`, `render-correctness`, `mesh-memory`, `project-window`)
- `editor/shader/ShaderBuilder.{h,cpp}`, `engine/core/shader/ShaderDataSerializer.cpp` (comment)
- `editor/Generator.cpp` (Play build errors), `editor/Factory.cpp` (`hasTextureRect`),
  `editor/Out.cpp`
- `editor/window/dialog/ExportWindow.cpp` (+ `ExportWindowShipping.cpp`),
  `editor/window/ConsoleWindow.{h,cpp}`
- `editor/mcp/tools/BuildTools.cpp`, `ScriptTools.cpp`, `PlayTools.cpp`, `editor/cli/CommandLine.cpp`
- `engine/core/manager/SceneManager.cpp`, `engine/core/Engine.cpp`, plus whichever main-scene
  readers the audit fixes
- `engine/CMakeLists.txt`, `engine/cmake/JasperEnginePaths.cmake` (new),
  `engine/libs/{meshoptimizer,thorvg,rmlui,lunasvg,astcenc,zstd,msdfgen,lexbor}/CMakeLists.txt`,
  `engine/libs/shadercompiler/embed_shaderlib.cmake`, `engine/jasperengine.py`,
  `CopyEditorEngine.cmake`, root `CMakeLists.txt` (new editor sources)
- Tests: `editor/tests/export/*`, `editor/tests/util/BuildDiagnosticsTest.cpp`,
  `editor/tests/engine/LoadingSceneStartTest.cpp`, `editor/tests/shader/HlslBytecodeTest.cpp`,
  `editor/tests/GeneratedScenesTest.cpp`, `engine/tests/core/shader/ShaderDataSerializerTest.cpp`,
  `tools/mcp/tests/test_export_pipeline.py` (new), `test_build.py`, `test_build_method.py`,
  `tools/web/browser_gate.py`
- Docs: `docs/shipping.md`, `docs/mcp/guide/shipping.md`, `docs/mcp/guide/lua-api.md`,
  `docs/testing.md` (new test file in the lists, if it lists files)

## 13. Cross-feature dependencies

- **`mobile-shipping`.** It edits `chooseBuildMethod` (removes AdMob, adds the native-plugin
  reason) and the Android and Apple writers, where this feature turns every `writeIfChanged` into
  `writeOrFail`. Merge order: whoever lands second rebases onto the other. The helpers are small,
  and `mobile-shipping` should use `writeOrFail` and `copyOrFail` for its new privacy-manifest
  writes once this lands.
- **`build-toolchain`.** Exec bits on `gradlew` and `jasperengine.py`, and the `;`/`#` title
  escaping in apply.cmake and the generated CMake, touch `copyEngine`, `mirrorTree` (permission
  copy) and `writeExportSettingsScript`. `jasperengine.py` on Windows with Emscripten 6 touches the
  same script as 6.11. Coordinate: `mirrorTree` must preserve source permissions (std
  `copy_file` does on POSIX), and `build-toolchain`'s test should cover it through `mirrorTree`.
- **`unicode-text`.** Its project-walk guards and UTF-8 path rules apply to the new walker:
  `exportfiles::walk` uses `FileUtils::pathToUtf8` from the start, so the walkers it replaces drop
  out of `unicode-text`'s list of `.string()` sites.
- **`scene-integrity`.** It owns Save As for never-saved scenes and "Save All for never-saved
  scenes". Build Settings' **Save…** button (6.6) calls that dialog. Its "scenes stay inside the
  project" does not conflict with 6.3.
- **`mesh-memory`.** It may remove `MAX_SUBMESHES` or change capacity counting (52179c98a). 6.7
  is written for either outcome, and T9 uses `MAX_EXTERNAL_BUFFERS`.
- **`agent-authoring`.** "Export shader overrides editable without a build" edits Build Settings'
  shader section and the project shader overrides. 6.5.1's filtering of additions must stay in
  `resolveShaderKeys`, whatever the UI becomes.
- **Shader-variant features** (`render-correctness`, `draw-perf` directional-only variant,
  `sprite-batching`, `water-reflections`, `water-boats`, `ssr-probes`). They add variant bits
  through `ShaderVariantCollector`. This feature changes only which entities are walked (6.5.3)
  and which forks are dropped (6.5.2), so there is no conflict beyond `Project.cpp` merges.
- **`platform-lifecycle`.** It edits the web key map and audio unlock in
  `engine/platform/emscripten`. No overlap with apply.cmake, but both touch the web gate if it adds
  tests there.
- **`code-editor-git`.** No overlap.

## Deviations

Recorded during implementation (branch `dc/export-pipeline`).

1. **Capacities of script-only prefabs (6.7, F8, Doriax b1df76163): not implemented.** Lead
   decision (LEDGER.md, "Capacities"): `mesh-memory` removes the fixed component capacities, so the
   counting has nothing left to size. T9 and acceptance criterion 9 are dropped with it.
2. **Failure wording.** The Build dialog prefixes every failure with "Build failed:", so the
   exporter's own messages say what failed instead of repeating it: "Compiling failed with N
   error(s). First: <file>:<line>: <message>" and "CMake configuration failed with ...". A failure
   with no recognised error line names the last output line. Shader build failures say "<n>
   shader(s) failed to build. First: <shader> (<language>): <reason>".
3. **The Play-time C++ build in MCP (`compile_scripts`, `play` start).** These tools keep
   `ProjectChecks::summarizeCppBuild`, an existing parser with its own deduplication and limits
   (60 errors, 30 warnings), and do not take `BuildDiagnostics`. The Generator does use
   `BuildErrorLog` to repeat the errors at the end of the Play build's log. MCP's `build_project`,
   the exporter, the Console's level and the CLI share `BuildDiagnostics`.
4. **The start scene in Build Settings.** Scenes In Build lists saved scenes only, and the start
   scene is chosen from it, so a never-saved scene cannot be the start scene (Unity's untitled
   scenes are not in its build list either). Never-saved scenes still ship (6.6), with the warning
   and the **Save...** button.
5. **Base scene.** Covering the start scene leaves frames with no main scene. To make the loading
   scene clear the frame and own the swapchain redirect, `Engine::getBaseScene()` (the main scene,
   else the bottom running scene) was added. `RenderSystem::isBaseLayer`, `isFixedResolutionActive`,
   `updateSwapchainRedirect` and `Engine::endCompositeFramebuffer` use it. Without it, the loading
   scene would draw as a layer over an uncleared frame.
6. **`webTreePathRefusal`** (the Windows `%` refusal walk) does not skip earlier exports. It only
   refuses paths with `%` or `"`. A false refusal there names the folder, and moving the earlier
   export out of the project (which the build now asks for) clears it.
7. **The web gate with a special path (T19)** was not added. The quoting of `--preload-file` and
   `--pre-js` is checked by reading the generated flags (6.12) and by the normal web build. T18
   (configure in "My Game @1 [x]") covers the globs.
8. **Lean MCP rules.** No tool description changed. The new result text (the refusal, the `W-...`
   codes) has the eval case `guard_build_output_inside_project` (tools/mcp/eval/cases.py), and the
   detail went to `docs/mcp/guide/shipping.md`. The agent guide is unchanged. The live eval was not
   run here: it needs Orquester-managed accounts. `test_grade.py` passes.
9. **Linked folders follow S68's walker.** This overrides 6.1 and rule 7.2. S68 (linked folders,
   now on `main`) owns link following: `copyAssets`, `copyLua` and `copyCustomShaderSources` use
   its `Exporter::walkTree`, which handles cycles, the export's own folder and the link budget. On
   top of it this feature adds three things to that walker: the earlier-export skip, failed copies
   and removals failing the export, and a root that cannot be listed failing it. That walker keeps
   S68's `skip_permission_denied`, so an unreadable folder there is skipped without
   `W-UNREADABLE-FOLDER`. `exportfiles::walk` (generated sources, C++ scripts, artifacts) does not
   enter linked folders.
10. **One shader-key function with S64.** S64 (shader lists for closed scenes) landed first, with
    `Project::collectBuildShaderKeys` and `finishBuildShaderKeys` in `editor/project/
    ProjectShaderLists.cpp`. This feature's dead-fork filter moved into those two functions (an
    optional `deadForks` out-parameter), so there is a single implementation. The exporter, Build
    Settings, `get_build_info`/`generate_shaders`, Play's startup and the standalone cache all go
    through it.
11. **T17 (prebuilt web favicon in the slow lane)** was not added: the MCP suite has no prebuilt web
    build. The prebuilt `finalizeWeb` and the Compile path share `Exporter::writeWebFavicon`, which
    T16 covers through the source export.
12. **Test adjustments.**
    - T1 uses a binary data file (`ui/level.bin`). A texture would ship cooked, so its source copy
      never runs.
    - T2's walk order is "a folder's files, then its folders, each in name order"
      (`ExportFiles.h`).
    - T10 sets a loading timeout of 0, because unit frames do not draw.
    - T9 (capacities) is dropped with deviation 1.
    - The CLI red control and the special-path red control run against `main`'s export code and
      engine CMake files, applied as one patch on the build box.

