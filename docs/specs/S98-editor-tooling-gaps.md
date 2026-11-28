# S98 — Editor, MCP and tooling gaps from the JasperArcade port: scenes as data in every build, what ships, Android and iOS packaging, source files the editor leaves alone, editor cost, tool fixes

**Status: implemented** on `feature/s98-editor-tooling-gaps` (WP-1..WP-7 and the integration). What
the code does where it differs from this text is in "Deviations" at the end.

Arcade engine-gaps program (S92-S98). Source: the "Editor, MCP and tooling" section of the port's
ledger, `JasperArcade-Ported/docs/port/ENGINE_LIMITATIONS.md` (E1-E47), written against engine
commit `7f48d7e8d` (2026-10-30). Every item was checked against this tree (`82ccd7c6a`); paths are
relative to the repository root, `PORT/` is `/var/lib/orquester/workspaces/jaspersito/JasperArcade-Ported`.
"Verified" in §3 means read at the cited lines; "inferred" means deduced from code, not run;
"unverified" means an AC settles it. The port's own tools (`tools/je/je.py`, `tools/gen`,
`tools/unity2je`, `tools/android/build_apk.sh`) are game-side: an item about them is engine work only
where the engine behaviour underneath is wrong.

## 1. Goal

The port shipped 11 games, but the build and the editor cost it hours and patches:
- a 2 h 24 min Android build, about 2 h of it single-threaded scene functions (E23);
- exports that rewrote scenes (E25) and were not reproducible (E27);
- an APK that silently lost every folder starting with `_` (E28);
- a debug APK compiled at -O0 (E29);
- an export that ships every asset, used or not (E24);
- an editor that re-saves scenes nobody edited (E18), adds opened test scenes to the build (E8) and
  logs hundreds of lines of noise (E12/E42, E15, E39, E40).

S98 removes these at the engine level. Every build reads its scenes as data, the same files the
prebuilt player reads. The user decides what ships. The Android and iOS packages are right without
patches. The editor writes a project file only when the user changed it. Tools that agents call
report failures as failures. Each fix ships a test that fails without it.

## 2. Scope

**In scope.** Every E item with a MISSING or PARTIAL verdict in §3 that S98 owns:
- **Export pipeline**: cooked scenes and prefabs in Compile builds (E23), no source writes on export
  (E25), deterministic output (E27 guard), the CMake glob (E46), stale output files (E47).
- **What ships**: asset stripping, Always Include and Exclude rules (E24); one shader copy (E26).
- **Packaging**: Android (E28-E32) and the prebuilt iOS player's orientations (E4).
- **MCP tools**: `create_prefab` overwrite (E2), `eval_lua` timeout error (E17, E43), `check_shader`
  errors (E34), the cost of `create_particles` calls (E37), `wait` at time scale 0 (E44), and GPU
  timing from Lua (E45 leftover).
- **Editor source-file hygiene**: Stop writes no scene (E18), opening a scene does not add it to the
  build (E8), prefabs follow their `.material` files (E3).
- **Editor cost**: Scene-view gizmos without system entities (E12, E42), per-frame cost with large
  scenes (E11), imports that never block MCP (E21), no model errors while an import is pending (E15).
- **Import noise and defaults**: one summary line per model import (E39), Material Creation Mode
  None (E40), and an assets directory for new projects (E38).

**Out of scope**
- **Owned by sibling specs**:
  - E35, the PSB importer: S94 (R45), as the brief's default.
  - E36, a capsule with half height 0: S96 REQ-24 takes the whole item (`S96-physics-gaps.md:295`,
    §8 there).
  - E41's `mesh.sortPriority`: S94 R48 owns sort priority per renderer, which is the same key.
- **A `.mesh` asset** (E1's embedded custom-mesh buffers): a format feature that belongs with S53's
  mesh pipeline. Recorded as follow-up F1 (§9); brought into scope as WP-7 (Deviations).
- **The game-side items** (§3): no engine change.
- **Play Asset Delivery in source exports (Gradle asset-pack modules)**: REQ-16 covers prebuilt App
  Bundles, the route a Lua-only game such as the arcade takes. Recorded as follow-up F2.
- **Do-not-copy list**: nothing on it is involved, and no OpenEngine code is taken.

## 3. Current state

### 3.1 Triage

Counts: 4 FIXED, 15 PARTIAL, 18 MISSING (3 of them owned by S94/S96), 7 GAME-SIDE,
3 NOT-A-BUG/RETRACTED. Total 47.

| Item | Verdict | Evidence (current tree) | What S98 does |
|---|---|---|---|
| E1 scenes embed mesh buffers and matrices | PARTIAL | Derived world matrices are no longer written: `cfc5381a1` (S29 R20, `TransformPhysicsReflect.cpp:31-36`). A custom mesh still writes its buffers (`StreamComponents.cpp:65-79`, `encodeBuffers`) | Nothing; F1 (`.mesh` asset) |
| E2 `create_prefab` cannot overwrite | MISSING | Refuses any existing path: "prefabs are never overwritten" (`editor/mcp/tools/PrefabTools.cpp:730-734`) | REQ-21 |
| E3 prefabs embed material copies | PARTIAL | Scenes re-read linked `.material` files at open (`Project.cpp:5063-5094`); hot reload walks scene links only (`Project.cpp:1095-1122`). No prefab code reads `.material` (`editor/project/ProjectPrefab*.cpp`, `StreamPrefab.cpp`), and exports write the embedded copy (`Factory.cpp:1347`) | REQ-29 |
| E4 iOS orientation | PARTIAL | Source export rewrites `UISupportedInterfaceOrientations` from Player > Resolution and Presentation (`9e58d340c`; `Exporter.cpp:5680-5695`). The prebuilt iOS template still lists Portrait (`engine/platform/apple/ios/player-template/Info.plist:61-66`) and `fillIosTemplate` never rewrites it (`editor/platform/IosProject.cpp:114-122`) | REQ-20 |
| E5 software rendering in headless | GAME-SIDE | A machine limitation (ledger) | — |
| E6 Android/iOS export from this machine | RETRACTED | The ledger strikes it. Also, `build --platform android` now packages a prebuilt APK/AAB with no Gradle or NDK (`cd600cc8b`, `editor/cli/ShippingCommands.cpp:147-158`) | — |
| E7 Game view aspects, `--screen` | FIXED (aspect) / GAME-SIDE (window) | `configure_game_view` takes any `resolution` "WxH" (`8fc9ba6ba`, `ViewTools.cpp:2048`). The window size in `settings.yaml` (`AppSettings.cpp:404-411`) is the harness's to write (`PORT/tools/je/je.py:274-275` writes it only once) | — |
| E8 opening a scene adds it to project.yaml | MISSING (Unity parity) | `open_scene`: "adding it to the project if unlisted" (`SceneTools.cpp:2036`). `project.yaml` `scenes:` is the build list (`Stream.cpp:3384-3441`; `edit_build_scenes`, `ProjectTools.cpp:1167`). Unity never adds an opened scene to Build Settings | REQ-28 |
| E9 'Cannot delete entity ... locked child' | GAME-SIDE | The converter names a locked child without its owner (`DeleteEntityCmd.cpp:577-595`); the delete still works | — |
| E10 `require` in the console gives a fresh copy | NOT-A-BUG | `eval_lua` runs in the game's state and `require` shares `package.loaded` (`PlayTools.cpp:1372, 1415-1421`); a script component's chunk is per instance by design (`LuaBinding.cpp:1089-1092`), reached with `script(ref)` | — (the `a.b` vs `a/b` double load at `LuaBinding.cpp:847-893` is noted to S95) |
| E11 editor ~1 fps with 1,400 entities | MISSING, cause unverified | The Hierarchy has no list clipper (`editor/window/HierarchyWindow.cpp`, no `ImGuiListClipper`). The Scene view updates gizmos per entity every frame (`SceneRender3D.cpp:1548`, `:1640-1700`). `play` has no option that skips the editor panels (`PlayTools.cpp:4314-4325`) | REQ-31 (measure first) |
| E12 / E42 'No free system entity slot [1..1000]' | MISSING | The range is 1..1000 (`engine/core/ecs/EntityManager.h:45-48`; error at `:211`). Scene-view gizmos take system slots per object: light 2, camera 2, sound 1, body 1 (`SceneRender3D.cpp:224-266`), plus bones, joints and particles (17 `ScopedDefaultEntityPool` sites in `editor/render/`) | REQ-30 |
| E13 game-time waits time out at ~125 s | GAME-SIDE | `wait` with `game_seconds` has no wall-clock cap; it stops only after 60 s without progress (`PlayTools.cpp:125, 4014-4028`). The 125 s is the harness's `seconds + 120` socket timeout (`je.py:616-619`) | — |
| E14 `get_scene_tree` truncated silently | NOT-A-BUG | It returns `truncated: true` and the text says "... truncated at N GameObjects (raise limit, or pass root/depth)" (`EntityTools.cpp:402-416`; in the base since `ca5a85495`) | — |
| E15 'Model load failed' before the FBX import ends | PARTIAL | Models now appear once the import lands (`Project.cpp:1198-1250`, `retryFailedModelLoads`). A load that runs before it still logs "Model file not found" (`MeshSystem.cpp:1780`) and "Model load failed, not retrying" (`:7650`) | REQ-33 |
| E16 console keeps entries across Play | GAME-SIDE | `play` returns `console_cursor_at_start` (`PlayTools.cpp:1465`), and `read_console` takes `since` (`ConsoleTools.cpp:526`). The harness never passes it (`je.py:697-704`). A second route would duplicate this one | — |
| E17 / E43 `eval_lua` stops after 5 s | PARTIAL | `timeout_seconds` (0.1-60, default 5) exists, and existed in the base (`PlayTools.cpp:4437, 2095`; base `:1877`). The error names no fix: "timed out after 5.0 s (infinite loop?)" (`:1357`) | REQ-24 |
| E18 the editor re-saves scenes at Stop | PARTIAL | S63 and S64 made saves byte-stable and shader lists computable. Stop still merges discovered shader keys, marks the scene modified (`Project.cpp:7122-7148`), and writes it: a full save of the main scene, or the keys into the file (`saveLoadedSceneOnStop`, `:7902-7921`; `saveSceneShaderKeys`, `:7925`) | REQ-27 |
| E19 UserSettings not per session | FIXED (reset) / GAME-SIDE (path) | The `user_settings` tool with `clear` (`2cef5c440`, `DataTools.cpp:171-186`), and Edit > Clear User Settings. The data lives per project (`Project.cpp:9825-9836`); harness sessions share one project directory | — |
| E20 'free' aspect needs `--no-game-view` | GAME-SIDE | The harness's `play` sets `aspect: project` first (`je.py:594-596`); Play start sets no aspect | — |
| E21 MCP blocked >10 min during the first import | PARTIAL, cause unverified | The scan is a Background job (`Project.cpp:1474-1514`), but main-thread paths take the same `fbxImportCallMutex` (`:1176, :1303` vs `:1505`) and convert synchronously (`ModelLoadCmd.cpp:627`, `WorldTools.cpp:1187`, `McpValues.cpp:3058`, `AssetTools.cpp:2200`) | REQ-32 |
| E22 regenerating rewrites sprite files | GAME-SIDE | `PORT/tools/gen/genlib.py:233-234` runs `uisprites.main()` | — |
| E23 huge scene functions in Compile builds | PARTIAL | The prebuilt player loads cooked `.jscn`/`.jpfb` (`a63d292be`, `engine/player/PlayerMain.cpp:257, 369-384`). Compile builds still emit one `create_<scene>()` per scene (`Factory.cpp:2852-2902`), compiled at full optimisation, with no split (`engine/cmake/JasperUnityBuild.cmake:24-27` leaves generated files out of unity batches) | REQ-1 to REQ-3 |
| E24 no export exclude, ships unused imports | MISSING | Only `.`-folders, `build`, `Recordings`, `EditorSettings` are skipped (`Project.cpp:5983-5992`). Every FBX ships its import: "Scripts may load any of them, so none can be left out" (`Exporter.cpp:3768-3783`) | REQ-8 to REQ-10, REQ-12 |
| E25 export re-saves scenes | MISSING | `loadAndSaveAllScenes` saves prefab stages (`Exporter.cpp:2462`) and every loaded scene, unsaved edits included (`:2508`). `saveSceneToPath` also writes `project.yaml` (`Project.cpp:8325-8332`) | REQ-4 |
| E26 shader sources copied twice | MISSING | `copyCustomShaderSources` copies every `.vert/.frag` to `assets/<project-relative path>` (`Exporter.cpp:3988-4017`); `copyAssets` has already copied them from the assets dir, giving `assets/assets/shaders/...` | REQ-11 |
| E27 non-deterministic export (`solidBoxDensity`) | FIXED | `3615d4139` gives `solidBoxDensity = 1000.0f` (`RigidbodyComponent.h:132`) and fixes three other fields; `ComponentDefaultsTest.cpp:38-43`. No other primitive field without an initialiser is written by the generator (the only hit, `PointRenderData::size/rotation`, `PointsComponent.h:33-34`, is a runtime cache) | REQ-5 (a determinism guard) |
| E28 AGP drops `_` asset folders | MISSING | The template sets only `noCompress` (`engine/workspaces/androidstudio/app/build.gradle:42-44`); no `ignoreAssetsPattern` anywhere; no check. The prebuilt path links assets with `aapt2 link -A` (`AndroidPackager.cpp:384-392`); whether aapt2 drops them is unverified | REQ-13 |
| E29 debug native -O0, no suffix, signing or AAB | PARTIAL | Prebuilt Android has AAB, keystore and alias (`5f8c42bdd`, `Project.h:822-828`). The source template has only a `release` build type (`build.gradle:90-101`), no `CMAKE_BUILD_TYPE`, no suffix and no signing; S67 records the debug-key signing (`S67-packaging-tests.md`, Deviations) | REQ-14 to REQ-16 |
| E30 NDK not pinned or installed | PARTIAL | `ndkVersion = "28.2.13676358"` is pinned (`2b6577e73`, `build.gradle:39`). CMake is not pinned; `docker/android.Dockerfile:22-23` installs no NDK or CMake ("AGP installs any missing NDK/CMake here") | REQ-17 |
| E31 no `--platform` on `export`; Vulkan in the Android preset | PARTIAL | The Android source backends are GLES3 only (`2be29f169`, `ShippingTypes.cpp:99-108`), for MCP and the window. `export` has `--backend` and no `--platform` (`editor/cli/CommandLine.cpp:351-372`) | REQ-18 |
| E32 one launcher icon density | MISSING | `launcherIcon` becomes one `res/drawable/ic_launcher.png` (`Exporter.cpp:5108-5119`), as in the prebuilt packager (`AndroidPackager.cpp:363-374`); the template's `mipmap-*dpi` icons stay; no round icon | REQ-19 |
| E33 Android Back does nothing | GAME-SIDE | A game decision (the Unity game ignores Back too) | — |
| E34 `check_shader` not an error | MISSING | `call->ok(...)` with `failed` in the result (`AssetTools.cpp:3836-3837`) | REQ-22 |
| E35 no PSB | MISSING | No `.psb` in the texture list (`editor/util/Util.h:34`) | S94 R45 |
| E36 capsule half height 0 | MISSING | `WorldTools.cpp:1720-1724`; `JoltShapeBuilder.cpp:366-370` | S96 REQ-24 |
| E37 `create_particles` slow in big scenes | PARTIAL, cause unverified | One undo step, no scene refresh (`WorldTools.cpp:5912-6071`). The game calls it once per system (`PORT/tools/unity2je/u2j_runtime.py:669-675`) instead of through `batch` (`CoreTools.cpp:938`, 200 calls). Where the growth with scene size comes from is not known | REQ-25 (measure first) |
| E38 new projects have no assets dir | MISSING | `defaultAssetsDir = "."`, `defaultLuaDir = "."` (`Project.h:1670-1671`). Every template has `luaDir: .` and no `assetsDir` (`editor/resources/templates/*/project.yaml`) | REQ-36 |
| E39 FBX warnings per texture | MISSING | "Texture not found" (`editor/util/GlbBuilder.cpp:389`) and "Texture tiling/offset is not imported" (`:505`), one Console line each (`Project.cpp:1149-1151`) | REQ-34 |
| E40 no import setting to skip materials | MISSING | `ModelImportSettings.h` (S53 `modelImporter`) has no material option | REQ-35 |
| E41 `mesh.sortPriority`; texture settings in `create_entity` | PARTIAL | The alias is missing (`McpValues.cpp:3988-4001`, one-to-one aliases only): S94 R48. Texture values already take `{"path", "min_filter", ...}` in `create_entity` (`McpValues.cpp:1909-1920`, in the base) | — (alias: S94) |
| E44 `wait {game_seconds}` at time scale 0 | MISSING | Without `frames`, game time must advance; after 60 s the wait ends `stalled` as an error (`PlayTools.cpp:4016-4027, 4142-4147`) | REQ-23 |
| E45 no GPU timing | FIXED, Lua leftover | S15: per-pass GPU timers on every backend (`182c27b34`, `engine/libs/sokol/sokol_gpu_timer.h:7-27`, `SokolPassRecorder.cpp:450-524`), `get_gpu_passes` (`ProfilerTools.cpp:1264`), `Profiler.frameStats().gpuMs` (`ProfilerClassesLua.cpp:96-121`). Lua cannot turn GPU timing on and gets no per-pass list. `JASPERENGINE_FRAME_STATS` stays byte-identical on purpose (S15 AC9) | REQ-26 |
| E46 GLOB without CONFIGURE_DEPENDS | MISSING | `engine/CMakeLists.txt:2597` | REQ-6 |
| E47 stale files in an export folder | PARTIAL | Prebuilt builds stage and swap (`a63d292be`, `ExporterPrebuilt.cpp:912-930`). Compile desktop builds copy `assets/` and `lua/` file by file over the old output (`Exporter.cpp:1983-1993`, `export/ExportFiles.cpp:150` `overwrite_existing`) | REQ-7 |

### 3.2 How builds get their scenes today (verified)

- **Compile** (C++ scripts, `ProjectBuild.cmake`, native plugins, or chosen):
  - `Exporter::loadAndSaveAllScenes` (`Exporter.cpp:2439-2545`) loads every scene and saves it. The
    save regenerates `.jasperengine/generated/scene_<name>_<id>.cpp` (`Project::writeSceneSource`,
    `Project.cpp:8375, 8405-8412`).
  - The exported engine CMake globs `project/*.cpp` (`engine/CMakeLists.txt:2597`) and compiles one
    function per scene and prefab.
  - The generated `main.cpp` registers each stack with `SceneManager::registerScene`
    (`Generator.cpp:2835`).
  - The project's own `CMakeLists.txt` compiles the same files as `FACTORY_SOURCES` when it is not
    the editor's Play plugin. Play builds only the C++ scripts and `initScripts`
    (`PORT/CMakeLists.txt:227-247`), so Play never runs scene C++ (inferred).
- **Prebuilt** (S30, picked by `chooseBuildMethod`, `ExporterPrebuilt.cpp:142-194`):
  - `SceneCooker::cookProject` writes `scenes/<id>.jscn` and `prefabs/<id>.jpfb`
    (`Exporter.cpp:2531`).
  - `PlayerMain` registers the same stacks from `game.json` and builds them with
    `CookedSceneLoader` (`PlayerMain.cpp:243-262, 365-384`).
  - Its comments say it mirrors "a Compile build's init" for layers, audio and native plugins
    (`:356-363`).
- **C++ scripts** are instantiated from `ScriptComponent` data after a scene is built:
  - `createCppScripts` and `bindCppScriptProperties` are generated once per script class, not per
    GameObject (`Generator.cpp:695-760`).
  - `ScriptEntry` holds `className` and the property values (`ScriptComponent.h:21-41`).
  - So nothing in a scene needs code generated per GameObject.

### 3.3 The port's workarounds (PORT/tools/android/build_apk.sh)

- `:83` exports with `--backend opengles` (E31).
- `:87` fails on any `[ERROR]` except the system-slot line (E42).
- `:93-101` seds `solidBoxDensity` (E27, now fixed).
- `:116` touches `CMakeLists.txt` when the list of `project/*.cpp` changes (E46).
- `:130-132` adds a debug build type with `applicationIdSuffix` and `-DCMAKE_BUILD_TYPE=Release`
  (E29).
- `:137-142` sets `ignoreAssetsPattern` without `<dir>_*` (E28).
- `:190-196` checks that every exported file is in the APK (E28).
- `RELEASE.md` §5 restores every scene the export re-saved (E25), and keeps a writable SDK copy for
  the NDK and CMake (E30).

## 4. Reference

- **Unity behaviour** (the parity bar, from Unity's manual):
  - Build Settings lists the scenes in the build; opening a scene never adds one.
  - Only assets the build's scenes reference, plus `Resources/` and `StreamingAssets/`, ship.
  - `PrefabUtility.SaveAsPrefabAsset` overwrites a prefab in place and keeps its GUID.
  - Model Importer > Materials > Material Creation Mode: None.
  - The Android Player settings generate every launcher icon density and a round icon from one
    image, and sign the release with a keystore whose passwords are not saved.
  - A debug ("Development Build") APK is still compiled optimised (IL2CPP).
  - App Bundles move large content into install-time asset packs (Play Asset Delivery).
  - `FrameTimingManager` gives GPU frame times to scripts.
- **The prebuilt player** (`engine/player/PlayerMain.cpp`, `engine/core/serialization/CookedScene.h`)
  is the model for REQ-1: it already loads everything a Compile build needs except native code.
- No OpenEngine code. The do-not-copy list is not involved.

## 5. Requirements

### Functional — every build reads its scenes as data (E23, E25, E27, E46, E47)

- **REQ-1 Compile builds load cooked scenes and prefabs.**
  - A Compile build (desktop, Android, iOS, web source exports and `build_project method=compile`)
    ships `scenes/<id>.jscn` and `prefabs/<id>.jpfb` from `SceneCooker`, as a prebuilt build does.
  - It loads them with `CookedSceneLoader`.
  - No `create_<scene>()`, `resources_<scene>()` or prefab factory is generated or compiled.
  - The generated C++ of a build is the C++ script registration (REQ-3) and the project's
    native-plugin glue. Its size grows with the number of script classes, not with GameObjects.
  - `Factory`'s scene and prefab emitters (`Factory::createScene`, `createPrefab`,
    `createPrefabHeader` and the per-component emitters only they use) are deleted, not left
    unused.
- **REQ-2 The project's own CMake build reads the same content.**
  - The standalone target of the generated project `CMakeLists.txt` (the IDE build, not the Play
    plugin) runs from cooked content.
  - The editor writes that content to `.jasperengine/cooked/` when a scene or prefab is saved, in
    place of `.jasperengine/generated/scene_*.cpp`.
  - The editor deletes stale `scene_*.cpp`/`prefab_*.cpp` (and their headers) from
    `.jasperengine/generated/` when it opens a project. That folder is the editor's cache.
- **REQ-3 C++ scripts on cooked scenes.**
  - After `CookedSceneLoader` builds a scene, and after it instantiates a prefab, a Compile build
    runs the generated `createCppScripts` and `bindCppScriptProperties` on the new GameObjects.
  - Each script's lifecycle, messages and properties (values and GameObject pointers) behave as
    they do today.
- **REQ-4 An export writes no project source.**
  - Building writes only into the output folder and `.jasperengine/`. That excludes `.scene`,
    `.prefab`, `.material`, `project.yaml`, `ShaderWarmup.json` and files in the assets dir.
  - Scenes are cooked from what the editor holds.
  - An open scene with unsaved changes, or a Prefab Mode stage with unsaved changes, ships as shown
    and logs `W-SCENE-UNSAVED` naming it. This extends today's `W-SCENE-NOT-SAVED`,
    `Exporter.cpp:2522`.
  - The FBX imports an export needs (REQ-12) go to the import cache, which is `.jasperengine/`.
- **REQ-5 Deterministic output.**
  - Two exports of an unchanged project, from one editor build, give byte-identical output trees.
  - The only exceptions are the fields the content manifest declares as stamps (build time, and the
    build id if there is one).
  - This covers cooked files, generated C++, shaders and manifests, on every target.
- **REQ-6 New generated sources are seen.**
  - The project glob in `engine/CMakeLists.txt:2597` uses `CONFIGURE_DEPENDS`.
  - So an incremental Gradle `.cxx` or CMake tree picks up a C++ script added since its last
    configure.
- **REQ-7 No stale output.**
  - A Compile desktop or web build stages its output beside the destination and swaps it in, as
    prebuilt builds do (`ExporterPrebuilt.cpp:912-930`, S30 R7).
  - A file the build no longer writes is gone from the destination.
  - It uses the same swap function and the same refusals (no swap over a folder that is not a build
    output).

### Functional — what ships (E24, E26)

- **REQ-8 Asset Stripping.**
  - Project Settings > Player > Build gets **Asset Stripping**, with two values:
    - **Ship All Assets**: today's behaviour. It is the default when the key is absent.
    - **Referenced Only**.
  - It also gets **Always Include** and **Exclude From Build**, lists of project-relative globs.
  - Exclude wins over Always Include. Both apply under either mode.
  - The Project window's context menu gets **Exclude From Build** and **Always Include in Build**,
    which add or remove the selection's glob, with undo.
- **REQ-9 What Referenced Only ships.** The closure, over the S04 dependency index
  (`editor/asset/AssetIndex.h:59-66`), of:
  - the build's scenes;
  - the prefabs, materials, shaders, models, textures, audio, HTML/CSS and Lua they reference;
  - Lua `require` targets;
  - every string literal in a shipped Lua or C++ source that names an existing asset path;
  - Always Include.

  Lua scripts are always shipped whole. An FBX ships its import only if something in the closure
  references it.
- **REQ-10 Report what was left out.**
  - The Build Report (`editor/cook/BuildReport.*`) lists every stripped and every excluded asset,
    with its size and its reason (`unreferenced` or `excluded: <glob>`).
  - The `build_project` result gives `stripped_count`, `stripped_bytes` and the report path.
  - In a Development Build the content manifest also lists the stripped paths. A runtime load of one
    then logs "`<path>` was stripped from the build (Asset Stripping: Referenced Only): add it to
    Always Include", not a plain "not found". A release build carries no list.
- **REQ-11 One copy of each shader source.**
  - A custom `.vert/.frag/.glsl` ships once, at the project-relative path the runtime reads
    (`copyCustomShaderSources`).
  - `copyAssets` leaves shader sources out. When the assets dir is the project root, the two paths
    are the same file.
- **REQ-12 Imports follow what ships.**
  - An export imports only the FBX files it ships (REQ-9).
  - Play start (`Project.cpp:10741`, `importFbxModels`) also imports only the FBX that the build
    would ship.
  - Under Ship All Assets that is every FBX, as today.

### Functional — Android and iOS packages (E4, E28-E32)

- **REQ-13 No silently dropped assets.**
  - The source template sets `androidResources.ignoreAssetsPattern` to AGP's default minus
    `<dir>_*`: `!.svn:!.git:!.ds_store:!*.scc:.*:!CVS:!thumbs.db:!picasa.ini:!*~`.
  - The prebuilt packager passes the same pattern to `aapt2 link` (`--ignore-assets`) if aapt2
    applies a default.
  - After packaging, it compares the staged content list with the package entries. A missing file
    fails the build and names the file.
- **REQ-14 Debug packages are playable and installable side by side.**
  - The source template gets a `debug` build type whose native code builds `RelWithDebInfo`
    (`-DCMAKE_BUILD_TYPE=RelWithDebInfo`).
  - Android settings get **Debug Package Suffix**: `applicationIdSuffix` of the debug type, default
    empty. The prebuilt packager applies it when it signs with the debug key.
- **REQ-15 Release signing in source exports.**
  - When Android settings name a keystore (`keystorePath`, `keystoreAlias`, `Project.h:827-828`),
    the template's `release` build type gets a `signingConfig`.
  - It reads the passwords from `JASPER_ANDROID_KS_PASS` and `JASPER_ANDROID_KEY_PASS`, the
    prebuilt route's variables. Passwords are never written.
  - With no keystore, the release build is unsigned and the export log says so.
- **REQ-16 Play Asset Delivery for App Bundles.** A prebuilt AAB puts the game content
  (`assets/game`) in one install-time asset pack, `game_content`. The runtime reads it through
  `AAssetManager` at the same paths, so the runtime does not change. APK outputs stay single-module.
- **REQ-17 The build image holds the toolchain.**
  - `docker/android.Dockerfile` installs the NDK the template pins (`28.2.13676358`) and one CMake
    version.
  - The template pins that CMake version (`externalNativeBuild.cmake.version`).
  - A fresh container then downloads neither.
- **REQ-18 `export --platform`.**
  - `jasperengine-editor export --platform windows|linux|macos|android|ios|web` takes the backends
    from `sourceExportBackends` (`ShippingTypes.cpp:99-108`). For Android that is GLES3 only.
  - `--backend` still overrides.
  - An unknown platform exits 2 and lists the valid names.
  - Without `--platform` and `--backend`, the backends are all of them, as today.
- **REQ-19 Launcher icons at every density.**
  - From the Launcher Icon, both routes write:
    - `mipmap-{m,h,xh,xxh,xxxh}dpi/ic_launcher.png` at 48/72/96/144/192 px;
    - a round `ic_launcher_round` (a circular mask, same sizes);
    - `android:roundIcon`.
  - From the adaptive foreground and background, they write the 108 dp layers at 108/162/216/324/432
    px.
  - When an icon is set, the template's default `mipmap-*dpi` icons do not ship.
  - Resampling uses the engine's existing image resizer, with no new library.
- **REQ-20 iOS orientations in prebuilt builds.**
  - `fillIosTemplate` writes `UISupportedInterfaceOrientations`, its `~ipad` key and
    `UIRequiresFullScreen` from Player > Resolution and Presentation.
  - It uses the function the source export uses (`PlatformRules::iosPlistOrientations`). The source
    export's code moves into that shared helper, not into a second copy.

### Functional — MCP tools (E2, E17/E43, E34, E37, E44, E45)

- **REQ-21 `create_prefab` overwrite.** `create_prefab {overwrite: true}` on an existing `.prefab`:
  - replaces its content with the GameObject's;
  - keeps its GUID and `.meta`;
  - updates every open instance as an edit to the prefab does, with each instance's overrides kept;
  - is one undo step that restores the old content;
  - refuses a path whose file is not a prefab;
  - refuses an overwrite that would make the prefab contain itself.

  Without `overwrite`, the refusal's text names the parameter.
- **REQ-22 `check_shader` fails on a failure.** When any shader fails to compile, or a state finding
  is an error, `check_shader` returns `isError: true` with the same text and the same
  `structuredContent` (`results`, `failed`, `findings`). The CLI `call` exits non-zero.
- **REQ-23 `wait` at time scale 0.**
  - When a wait has only a `game_seconds` cap, and `Engine::getTimeScale()` stays 0 for 1 s of wall
    clock, the wait ends.
  - It ends successfully, with `reason: "time_scale_zero"`, the game time waited so far, and `next`:
    "the time scale is 0: wait with seconds or frames, or unpause".
  - A wait with `until`, `frames` or `seconds` is unchanged.
- **REQ-24 `eval_lua`'s timeout names the fix.** The timeout error says the limit that ran out and
  names the parameter and its maximum:
  - "timed out after 5.0 s: pass timeout_seconds (up to 60)";
  - for longer work: "run it as a coroutine in a script and wait with wait {until}".

  The 60 s cap stays: the call holds the main thread.
- **REQ-25 `create_particles` costs the same in any scene.**
  - The mean time of a `create_particles` call does not grow with the number of GameObjects in the
    scene. The budget is in "Performance".
  - The implementer first measures where the time goes (S15 CPU profiler, `capture_profile`) and
    records it in Progress.
  - No `items[]` form is added: `batch` is the bulk route.
- **REQ-26 GPU timing from Lua.**
  - `Profiler.gpuTiming`, read and write, turns GPU timing on and off: it holds the GPU timer lease
    `get_gpu_passes` uses (`SokolSystem.cpp:47-49, 1091-1106`).
  - While it is on, `Profiler.frameStats()` returns `gpuMs` and `gpuPasses`, a list of `{name, ms}`
    for the newest timed frame.
  - Where the backend or the build has no timer, `gpuTiming` stays false and the fields are absent.
  - It works in Development Builds and in the editor. In a release build, where the profiler is
    compiled out, it is false.

### Functional — the editor leaves source files alone (E3, E8, E18)

- **REQ-27 Stop writes no scene file.**
  - Stop never saves or patches a scene file. `saveLoadedSceneOnStop` and `saveSceneShaderKeys`
    lose their Stop callers.
  - Shader keys a Play session drew with, which the scenes' static collection lacks, are merged into
    the project's shader warm-up list (`ShaderWarmup.json`, S32). That happens whatever
    `recordDuringPlay` says.
  - The list is sorted and written (AtomicFile) only when it gains a key.
  - Builds read those keys whatever `includeInBuilds` says, because they replace what the scene
    files used to hold. `includeInBuilds` keeps governing warm-up at startup.
- **REQ-28 Opening a scene does not put it in the build.**
  - `open_scene`, and File > Open Scene on a `.scene` that `project.yaml` does not list, open it
    without adding it to `scenes:` and without writing `project.yaml`.
  - The workspace remembers it as open.
  - Build Settings shows it as not in the build, with **Add Open Scenes**.
  - `edit_build_scenes add` and the Build Settings window add it.
  - A scene that a listed scene uses as a child scene is in the build as today.
  - `create_scene` is unchanged: a new scene still joins the build.
- **REQ-29 Prefabs follow their `.material` files.**
  - A submesh linked to a `.material` file (`Material::name`, S04 deviation 1) takes the file's
    current values everywhere a prefab is read: Prefab Mode, instances when a scene opens, Play's
    templates, the cook and the export.
  - It does this as scenes do (`Project.cpp:5063-5094`).
  - A `.material` change refreshes open instances and stages through the hot-reload handler
    (`editor/reload/handlers/Materials.cpp`).
  - The prefab file is rewritten only when the prefab itself is saved.

### Functional — editor cost (E11, E12/E42, E15, E21)

- **REQ-30 Gizmos take no system entity per object.**
  - Scene-view and Game-view overlay gizmos take a number of system entities set by the number of
    gizmo kinds, not by the number of objects. That covers lights, cameras, sounds, bodies, bones,
    joints, animators, lattices and particles (the 17 `ScopedDefaultEntityPool` sites in
    `editor/render/`).
  - The kinds share renderers: one line batch per kind and one icon batch.
  - Clicking an icon still selects its GameObject, and hover and selection colours are unchanged.
  - The system range stays 1..1000 (D9).
- **REQ-31 Large scenes stay interactive.**
  - The Hierarchy draws only visible rows (a list clipper over the flattened, expanded tree).
  - The Scene view skips its gizmo and overlay updates in any frame it is not drawn. That includes
    the headless export, and Play with the Game view maximised.
  - The budget is in "Performance".
  - The implementer first profiles the 1,500-GameObject fixture (AC24) with the S15 CPU profiler,
    and fixes every editor-side scope above 1 ms. Findings go in Progress.
- **REQ-32 Imports never block the tool server.**
  - While a model import runs (the background scan, Play start, an export, `sync_files`), the main
    thread does not wait on `fbxImportCallMutex` or convert a model.
  - A main-thread path that needs a model whose import is queued or running does one of three
    things: it queues the import and uses the state it has (a model load waits, REQ-33); it waits in
    a job; or it returns `next` naming the import job.
  - Tools that need no model answer while the import runs.
- **REQ-33 A pending import is not a failure.**
  - A model load whose FBX import is queued or running is marked waiting. It logs no "Model file not
    found" or "Model load failed, not retrying". It loads when the import lands, as now.
  - One info line per batch says "Importing N models (first open): they appear when ready".
  - `editor_status` shows the import job with `done/total`.

### Functional — import noise and new projects (E38, E39, E40)

- **REQ-34 One line per model import.**
  - An import with warnings logs one Console warning per model, for example "Hall.fbx: 3 textures
    not found (Wall.psd, Floor.png, +1); tiling/offset not imported on 2 materials".
  - The full list is in the model's import report (`inspect_models` and the Model Importer Inspector, extended,
    not a new tool).
  - The warnings are deduplicated per model per import.
- **REQ-35 Material Creation Mode.**
  - The Model Importer gets **Material Creation Mode**: Import (the default) or None. It is the S53
    `modelImporter` key `materialCreationMode`, and `set_model_import_settings`
    `material_creation_mode`.
  - With None, the import creates no materials and resolves no textures, so it warns about none.
    Submeshes get the default material.
  - Changing the setting re-imports the model.
- **REQ-36 New projects have an assets folder.**
  - Every way to create a project (the project hub's templates, `mcp --create`, the `create` CLI if
    present) writes `assetsDir: assets` and `luaDir: scripts`, with the template's content under
    those folders.
  - A `project.yaml` without the keys still means `.` (`Project.h:1670-1671` stay), so old projects
    load unchanged.
  - The templates are regenerated with `tools/templates/make_project_templates.py`.

### Platform & backends

- **REQ-37** REQ-1 to REQ-3 hold for every export target (Windows, Linux, macOS, Android, iOS,
  HTML5) and backend. The cooked format is backend-neutral (S30 D2).
- **REQ-38** REQ-13 to REQ-19 cover both Android routes (source Gradle project, prebuilt packager)
  unless a requirement names one. REQ-20 covers the prebuilt iOS route.

### Performance

- **REQ-39 Scene load.**
  - Fixture: a 2,000-GameObject scene generated by the test, with meshes, sprites, lights, bodies,
    Lua and C++ scripts.
  - Its cooked load in a Compile desktop build takes at most 1.25 × the time the generated
    `create_<scene>()` takes on the same commit, before REQ-1 removes it. Measure both in one run
    and record them in Progress.
  - After the removal the check runs against a hard budget of at most 60 ms, Release, dev host.
    Report it through `perfSlack()`.
- **REQ-40 Build time.** No translation unit in an exported `project/` folder is longer than 5,000
  lines. This holds for any scene size, because generated code is per script class.
- **REQ-41 Editor frame.** With the AC24 fixture open (1,500 GameObjects, 300 of them with lights,
  bodies, cameras or particles, Hierarchy fully expanded, nothing selected):
  - The editor-side CPU scopes of a frame (panels, Scene-view gizmo updates; not the engine's scene
    update or the render) total at most 8 ms in a Release editor.
  - In Play with the Game view maximised, the Hierarchy and Scene view together take at most 1 ms.
  - Reported through `harness.over_budget`.
- **REQ-42 `create_particles`.** Fixture: 300 calls into a scene of 1,000 GameObjects. The mean of
  the last 50 calls is at most 1.5 × the mean of the first 50.
- **REQ-43 Disabled costs nothing.**
  - A project on Ship All Assets runs no dependency closure.
  - `Profiler.gpuTiming` false holds no timer lease.
  - A model with Material Creation Mode Import pays nothing extra.

### Editor UX (human)

- **REQ-44** The new settings use the Unity-like names given above:
  - Asset Stripping, Always Include, Exclude From Build (Project Settings > Player and the Project
    window context menu);
  - Debug Package Suffix (Android);
  - Material Creation Mode (Model Importer).

  Each edit is undoable. Each one appears in settings search (S25).
- **REQ-45** Build Settings shows a scene that is open but not in the build (REQ-28) greyed, with
  **Add Open Scenes**. The Build Report window lists stripped and excluded assets (REQ-10).

### Agent UX (MCP, Lua, CLI)

- **REQ-46** The MCP surface changes, each with an eval case in `tools/mcp/eval/cases.py` that fails
  without it (AGENTS.md "MCP text"):
  - `create_prefab.overwrite` (REQ-21);
  - `wait`'s `time_scale_zero` `next` (REQ-23);
  - `eval_lua`'s timeout text (REQ-24);
  - `open_scene`'s description, where "adding it to the project if unlisted" is cut and replaced by
    "an unlisted one stays out of the build (edit_build_scenes add)" (REQ-28);
  - `set_model_import_settings.material_creation_mode` (REQ-35);
  - `build_project`'s `stripped_*` fields (REQ-10).

  Asset Stripping and its lists are set through `set_project_settings` (reflection rows), with no
  new tool.
- **REQ-47** `docs/mcp/TOOLS.md` is regenerated. `tool_budget.py --check` passes, and
  `tools/mcp/budget.json` is lowered by what the cut text saves. The guides
  `docs/mcp/guide/shipping.md`, `prefabs.md`, `profiling.md` and `assets.md` get their lines. The
  agent guide (`McpInstructions.cpp`) gets nothing unless an eval case needs it.
- **REQ-48** Lua: `Profiler.gpuTiming` and `frameStats().gpuPasses` go in the Lua stub fragments
  (`engine/core/script/binding/stubs/`) and `docs/profiling.md`.
- **REQ-49** CLI: `export --platform` (REQ-18) is in `printUsage` and `docs/shipping.md`.

### Data & compatibility

- **REQ-50** New optional fields get defaults that keep old projects behaving as before, with no
  version bump:
  - `build.assetStripping` (absent = Ship All Assets), `build.alwaysInclude`, `build.exclude`;
  - `android.debugPackageSuffix`;
  - `modelImporter.materialCreationMode` (absent = Import);
  - the warm-up list's discovered keys. If `ShaderWarmup.json` cannot hold keys without pipelines, a
    `variants` array (absent = empty).
- **REQ-51**
  - Old scene files keep the `shaderKeys` they hold, and builds still read them. Nothing removes
    them, so no migration is needed.
  - Old `.jasperengine/generated/scene_*.cpp` files are cache and are deleted (REQ-2).
  - Exports of an old project still build: the cooked format is S30's, at its current minor.
- **REQ-52** `CookedSceneLoader` is now the only producer of scenes in builds. A component the
  cooker does not carry cannot ship, so the AllComponents round trip (`CookedScene.RoundTripEveryComponent`,
  `CookedScene.RoundTripAllComponentsFixture`) is a gate for this spec. Any component it misses is
  fixed in the cooker before REQ-1 lands. It does not become a Compile reason, because Compile
  builds read cooked content too.

## 6. Design constraints & decisions

- **D1 (E23): scenes as data in Compile builds; generated scene code is removed.**
  - Rejected: splitting the generated functions per N GameObjects, or building them at -O1.
    - Either keeps code growing with GameObjects. The port's scene_G (76,541 lines) would still be
      tens of thousands of lines per ABI, and build time would only shrink by a constant.
    - Either also keeps two scene producers. S30 named their drift its biggest risk
      (`S30-shipping.md` §9).
  - Taken: cooked files. Their size is close to the data. The code shrinks by every scene function
    (smaller `libgame.so` per ABI). Startup reads one binary file per scene, bounded by REQ-39.
  - A Compile build becomes the prebuilt player compiled from source with the project's native
    code. Sketch:
    - `PlayerMain`'s content setup (`PlayerMain.cpp:243-394`) moves into one engine function both
      mains call: `player::runGame(const GameContent&, const ProjectHooks&)`.
    - `ProjectHooks` is generated in the project. It is `{initScripts(Scene*, span<Entity>),
      registerLuaBindings(lua_State*), cleanupScripts(Scene*)}`, called after `setupStack` and after
      a prefab instantiate (REQ-3).
    - The prebuilt player passes empty hooks.
  - `game.json` and the stage layout are shared, so the content manifest is S30's.
- **D2 (E25): build what the editor shows; write nothing.** Unity asks to save before a build; an
  agent cannot answer a modal well. The export cooks from memory. A modified scene is announced
  (`W-SCENE-UNSAVED`), not saved.
- **D3 (E24): asset stripping.**
  - Old projects default to Ship All Assets, because an absent key must not change what they ship.
    New projects are written with Referenced Only (REQ-36 templates), as Unity strips.
  - Lua and C++ string literals count as references, so `loadTexture("ui/logo.png")` keeps
    `ui/logo.png`.
  - A computed path (`"level" .. n`) needs Always Include. The Development-build message (REQ-10)
    says so where the failure shows.
- **D4 (E26): ship shaders where the runtime reads them.**
  - `customShader` values are project-relative (`Exporter.cpp:3996-3999`), so the runtime path is
    the project-relative copy.
  - Only `copyAssets`' copy is redundant, and it is the one dropped.
  - Rewriting every `customShader` reference to an asset-relative path was rejected: it changes
    stored meanings.
- **D5 (E18): Play-discovered shader keys belong to the project's warm-up list, not to scenes.**
  - S32 made `ShaderWarmup.json` the project's list of "what Play used ships"
    (`ProjectShaderLists.cpp:451-462`). The Stop write into scene files predates it.
  - One project file that changes only when a genuinely new variant appears is Unity's tracked
    shader variants. Scene diffs nobody made are not.
- **D6 (E8): the build list is Unity's Build Settings.** `project.yaml` `scenes:` stays the build
  list and keeps its meaning. Opening no longer adds to it. Scene ids live in the scene file
  (`Stream.cpp:4521, 4771`), so an unlisted open scene keeps its id across sessions.
- **D7 (E2): overwrite keeps identity.** It keeps the GUID and the instances' overrides, as Unity's
  `SaveAsPrefabAsset` does. Delete-and-create breaks every reference by GUID.
- **D8 (E34, E44): a failed check is an error, a paused game is not.**
  - A compile failure is the answer the caller asked about, and `isError` is how MCP clients see it.
  - A wait into a game whose own pause menu set the time scale to 0 has not failed.
- **D9 (E12/E42): fewer entities, not a wider range.** The range cannot grow:
  `FIRST_USER_ENTITY = 1001` (`EntityManager.h:47`), and user ids up to any bound are saved in
  scenes, so a larger system range would collide with saved GameObjects. Shared gizmo renderers also
  remove per-object update cost (REQ-31).
- **D10 (E21, E15): imports are jobs everywhere.** One import scheduler, the S06 Background job that
  exists. Main-thread callers enqueue and wait in a job or a waiting state. A main-thread caller
  never holds the import mutex.
- **D11 (E29): Play Asset Delivery only where the editor packages.**
  - Prebuilt AABs are built by the editor's bundletool step (`AndroidPackager.cpp:447-460`), which
    it controls and tests.
  - Install-time packs are read through `AAssetManager` like base assets, so the runtime does not
    change.
  - Source exports keep one module (F2).
- **D12 (E45): the stats line stays.** `JASPERENGINE_FRAME_STATS` stays byte-identical (S15 AC9,
  parsers rely on it). GPU times reach agents through `get_gpu_passes` and scripts through REQ-26.
- **D13 (E16, E13, E14, E19, E20): one route each.** The cursor, the uncapped wait, the
  `truncated` flag and `user_settings clear` exist. A second way to do each would be a near-duplicate
  (AGENTS.md "MCP text"), so the harness changes, not the engine.

## 7. Acceptance criteria

"Red" names the run that fails without the change. MCP tests declare `CI = True` unless marked
slow (`JASPER_SLOW_BUILD=1`, `docs/testing.md` "Compiled and prebuilt game builds").

| AC | Verifies | Check (red control) |
|---|---|---|
| AC1 | REQ-1, REQ-40 | `test_build.py::test_desktop_build_and_run` (slow, compiled) is extended: the exported `project/` holds no `scene_*.cpp` or `prefab_*.cpp`, holds `scenes/*.jscn`, no file is over 5,000 lines, and the game shows its scene. Red: the scene sources are there |
| AC2 | REQ-1, REQ-52 | `test_build_parity.py::test_compile_and_prebuilt_match` (slow): one Lua-only fixture built both ways; screenshots match with `pngdiff.py --tolerance 8 --max-fraction 0.002` (S30 AC5), glcore. This guards REQ-1 rather than having a red control |
| AC3 | REQ-3 | `test_lifecycle_export.py::test_play_and_export_log_the_same_lifecycle` passes on cooked scenes, extended with a C++ script on a prefab instantiated at runtime and a GameObject-pointer property. Red: with hooks not called, no lifecycle lines |
| AC4 | REQ-2 | `ProjectCookCache.WritesCookedSceneOnSave` (`editor/tests/project/`): a save writes `.jasperengine/cooked/scenes/<id>.jscn` and no `scene_*.cpp`; opening a project with a stale `scene_x.cpp` deletes it. Red: the `.cpp` is written |
| AC5 | REQ-4 | `test_export_generated_sources.py::test_export_writes_no_source`: hash every file outside `.jasperengine/`, export (prebuilt, then a source export) with one open scene modified and one prefab stage modified; hashes are equal, the log has `W-SCENE-UNSAVED` twice, and the cooked scene holds the modification. Red: the scene, prefab and `project.yaml` change |
| AC6 | REQ-5 | `test_export_generated_sources.py::test_export_is_deterministic`: two source exports and two prebuilt builds of `tools/fixtures` AllComponents project into fresh folders; the trees are byte-equal but for the declared stamps. Red: set `solidBoxDensity`'s initialiser aside in a scratch build (or any random field) and the test fails (record the run) |
| AC7 | REQ-6 | `ExportTemplate.ProjectGlobConfigureDepends` (`editor/tests/export/`): the engine CMake project block contains `GLOB_RECURSE PROJECT_SOURCE CONFIGURE_DEPENDS`. Manual (recorded): a Gradle `.cxx` tree built, a C++ script added, `assembleDebug` links. Red: the keyword is absent |
| AC8 | REQ-7 | `test_build.py::test_compile_build_removes_stale_files` (slow, compiled): build into a folder holding `assets/old.png` from an earlier build; afterwards it is gone and a file outside a build folder is refused as S30 refuses. Red: `old.png` stays |
| AC9 | REQ-8, REQ-9, REQ-10 | `test_asset_stripping.py`: a project with a referenced texture, an unreferenced one, a texture named by a Lua literal, an FBX used by no scene, a `Resources`-like folder in Always Include and a glob in Exclude. Referenced Only ships exactly the first, third and fifth; the report lists the other three with reasons; `build_project` gives `stripped_count == 3`; a Development Build's player log says "was stripped from the build" when Lua loads the stripped texture. Ship All Assets ships all but the excluded. Red: everything ships |
| AC10 | REQ-11 | `test_export_generated_sources.py::test_shader_shipped_once`: `assetsDir: assets` with a custom shader; one copy of it in the output, at `assets/assets/shaders/x.frag`, and the exported player draws it (no shader error in the log). Red: two copies |
| AC11 | REQ-12 | `test_asset_stripping.py::test_play_imports_only_shipped_fbx`: Referenced Only with an unused FBX; Play start leaves it unimported (no `.glb` in the cache). Red: it is imported |
| AC12 | REQ-13 | `AndroidTemplate.IgnoreAssetsPatternKeepsUnderscoreDirs` (`editor/tests/export/`): the written `build.gradle` has the pattern without `<dir>_*`. `test_android_package.py::test_underscore_folder_ships` (slow, prebuilt, S67 route): `_Onboarding/a.png` is in the APK; deleting an entry from the staged package (test hook) fails the build naming it. Red: no pattern; the verify step is missing |
| AC13 | REQ-14, REQ-15 | `AndroidTemplate.DebugTypeBuildsOptimisedNative` and `AndroidTemplate.ReleaseSigningFromKeystore`: the written Gradle has the `debug` type with `RelWithDebInfo`, the suffix, a `signingConfig` reading the two variables, and no password text. Manual (recorded in Progress): `tools/build.sh android` of a fixture, `assembleDebug` native flags show `-O2`. Red: no debug block |
| AC14 | REQ-16 | `test_android_package.py::test_aab_has_install_time_pack` (slow, prebuilt): `bundletool dump manifest --module game_content` shows `dist:install-time`; `bundletool build-apks --mode universal` gives an APK whose `assets/game` is complete; `tools/android-emulator-test.sh` on it reaches the scene (S67 expect file). Red: no module |
| AC15 | REQ-17 | `tools/ci/check_android_image.sh` (new, run by `tools/build.sh android`): `sdkmanager --list_installed` in the image lists `ndk;28.2.13676358` and the pinned `cmake;<v>`, and the template pins the same. Red: neither is installed |
| AC16 | REQ-18 | `test_call_cli.py::test_export_platform_android`: `export --platform android` writes GLES3 shaders only; `--platform bogus` exits 2 listing the names. Red: unknown option |
| AC17 | REQ-19 | `AndroidIcons.WritesEveryDensityAndRound` (`editor/tests/export/`): a 1024 px icon gives the five mipmaps at their sizes, the round ones (corner pixel transparent) and `android:roundIcon`; the template defaults are absent. Both routes (source writer, `AndroidPackager` staging). Red: one drawable |
| AC18 | REQ-20 | `IosTemplate.OrientationsFromPresentation` (`editor/tests/platform/`): Landscape Left + Right gives exactly those two in both keys and `UIRequiresFullScreen` true in the prebuilt template output. Red: Portrait listed |
| AC19 | REQ-21 | `test_prefabs.py::test_create_prefab_overwrite`: an instance with an override, then `create_prefab overwrite` from an edited GameObject; the GUID is equal, the instance shows the new content and keeps its override, `undo` restores the old content. Without `overwrite` the error names it. Red: refused |
| AC20 | REQ-22 | `test_call_cli.py::test_check_shader_failure_is_error`: a fragment shader with a mismatched varying gives `isError` with `structuredContent.failed == 1`; the CLI exits 1. Red: `isError` false |
| AC21 | REQ-23 | `test_play.py::test_wait_game_seconds_time_scale_zero`: Lua sets the time scale to 0; `wait {game_seconds: 2}` returns within 3 s wall clock, not an error, with `reason == "time_scale_zero"` and `next`. Red: it ends after 60 s as an error |
| AC22 | REQ-24 | `test_play.py::test_eval_lua_timeout_names_parameter`: a busy loop with `timeout_seconds: 0.2` fails with text containing `timeout_seconds` and `60`. Red: "infinite loop?" |
| AC23 | REQ-25, REQ-42 | `test_particles_tools.py::test_create_particles_flat_cost` (perf, `harness.over_budget`): the REQ-42 fixture and ratio. Red: the first run on the current code (record the ratio; the ledger reports more than 2×) |
| AC24 | REQ-30, REQ-31, REQ-41 | `test_render_perf.py`: the generated fixture (1,500 GameObjects: 100 lights, 100 bodies, 50 cameras, 50 particle systems, the rest meshes). (a) The open writes no "No free system entity slot" line; `SceneRenderGizmos.SystemEntitiesPerKind` (`editor/tests/render/`) creates 500 lights and 500 bodies and counts at most 2 system entities per gizmo kind. (b) The REQ-41 budgets, through `capture_profile` scopes. (c) `pick_at` on a light icon's screen position has the light as `would_select`. Red: 9+ slot errors; the budgets fail on the current code (record) |
| AC25 | REQ-32 | `test_fbx.py::test_tools_answer_during_import`: a project with FBX files and the test hook `JASPER_TEST_FBX_IMPORT_DELAY_MS=5000` per file; during the import `editor_status`, `get_scene_tree` and `read_console` each answer in under 1 s; `open_scene` of a scene using a pending model returns and the model appears after the import. Red: calls wait for the import |
| AC26 | REQ-33 | Same file, `test_pending_import_logs_no_error`: a fresh copy (no cache) opens a scene with FBX models; `read_console {only_problems: true}` has no "Model file not found" or "not retrying"; one "Importing N models" info line; `editor_status` lists the job with `done/total`. Red: the two errors per model |
| AC27 | REQ-27 | `test_play.py::test_stop_writes_no_scene`: two open scenes, one played, a Lua script creating a material with a new variant; after Stop no `.scene` file changed (hashes) and `ShaderWarmup.json` gained the key; a second identical Play leaves `ShaderWarmup.json` unchanged; a build includes the variant. Red: the scenes change |
| AC28 | REQ-28 | `test_project_save.py::test_open_unlisted_scene_stays_out`: `open_scene` an unlisted file, `save_scene` it; `project.yaml` is unchanged, `edit_build_scenes list` shows it as not in build, `add` puts it in. Red: `project.yaml` gains it |
| AC29 | REQ-29 | `ProjectPrefabMaterials.LinkedMaterialWinsOverEmbeddedCopy` (`editor/tests/project/`): change a `.material` while the prefab is closed; Prefab Mode, a scene instance, the cooked `.jpfb` and Play's template all show the new colour, and the `.prefab` file is unchanged. Red: the old colour |
| AC30 | REQ-34 | `FbxImporterWarnings.OneLinePerModel` (`editor/tests/util/`): a fixture FBX naming 3 missing textures and 2 tiled ones logs one warning line with the counts; the import report lists all 5. Red: 5 lines |
| AC31 | REQ-35 | `FbxImporterWarnings.MaterialCreationNoneWarnsNothing` and `test_meshes.py::test_material_creation_mode_none`: no warnings, submeshes with the default material, the setting changes the import stamp. Red: unknown key |
| AC32 | REQ-36 | `test_asset_stripping.py::test_new_project_layout`: `mcp --create` gives `assetsDir: assets`, `luaDir: scripts`; an imported FBX lands at `.jasperengine/imported/<path under assets>`; an old fixture without the keys opens with `.`. `make_project_templates.py --check` (or a regenerated diff) is clean. Red: `luaDir: .` |
| AC33 | REQ-26, REQ-48 | `ProfilerLua.GpuTimingArmsTimers` (`editor/tests/engine/profiler/`, GPU suite `...Gpu` with `JASPER_REQUIRE_GPU()`): `Profiler.gpuTiming = true`, render 10 frames, `frameStats()` has `gpuMs` and a non-empty `gpuPasses`; `false` drops the lease. Red: no field |
| AC34 | REQ-46, REQ-47 | The eval cases of REQ-46, each run on the old surface (red) and the new one (green), with results in Progress (`tools/mcp/eval/README.md`, Orquester accounts). `gen_tool_docs.py --check`, `tool_budget.py --check`, `test_tool_conventions.py`, `test_tool_coverage.py` pass |
| AC35 | REQ-50, REQ-51 | `StreamProjectSettings.NewBuildKeysDefault` and the `tools/test-projects/upgrade-from-main/` project open and export unchanged (no new keys written on open). Red: n/a (a guard) |
| AC36 | REQ-44, REQ-45 | Manual, with screenshots from `tools/editor-shot.sh` on a scratch copy: Project Settings > Player shows Asset Stripping and the two lists; the Project window context menu items; Build Settings with a greyed open scene and Add Open Scenes; the Model Importer's Material Creation Mode; each edit undone with Ctrl+Z |

## 8. Dependencies & conflicts

- **Builds on:**
  - S30: the cooked format, `PlayerMain`, the staging swap, `game.json`;
  - S04: the dependency index and GUIDs;
  - S32: `ShaderWarmup.json`;
  - S64: the shader lists;
  - S53: `modelImporter` and the Build Report;
  - S15: the GPU timers;
  - S67: the Android end-to-end tests;
  - S06: Background jobs.
- **S94:** owns E35 (PSB, R45) and E41's `mesh.sortPriority` (R48).
  - If S94 touches `GlbBuilder.cpp` or `FbxImporter.cpp` for FBX materials (R-items on Unity
    material import), WP-3's REQ-34/35 edits there merge on top of S94's, append-only. The
    summary line goes in `Project::importFbx`'s output loop (`Project.cpp:1149-1151`).
- **S96:** owns E36 (REQ-24 there) and edits `WorldTools.cpp:1720-1724`. WP-3 edits only
  `createParticlesTool` in that file.
- **S95:** E10's module-name normalisation (`LuaBinding.cpp:847-893`) is noted to S95, not taken
  here. WP-3 adds `Profiler.gpuTiming` in `ProfilerClassesLua.cpp`, a file S95 does not list.
- **S92:**
  - may touch `SceneTools.cpp` (R21 there). WP-4 changes only the `open_scene` description and its
    handler's listing call, append-only.
  - S92's R20 (Game view stats overlay) is not touched here.
- **S93:** no overlap (S98 does not reshape `check`).
- **S97:** touches the Hierarchy (`HierarchyWindow.cpp`). WP-5's list clipper changes the row loop.
  Land S97's Hierarchy rows first or rebase onto them; the clipper must keep every row kind.
- **Merge hotspots in S98:**
  - `editor/Exporter.cpp`: WP-1 owns `loadAndSaveAllScenes`, `copyGenerated`,
    `collectDesktopArtifacts` and the staging. WP-2 owns `writeAndroidProjectSettings`, the icon and
    plist sections. WP-6 owns `copyAssets` and `copyCustomShaderSources`.
  - `editor/Project.cpp`:
    - WP-4: the Stop path (`:7100-7200`, `:7902-7960`) and the open listing;
    - WP-5: the FBX import functions (`:1160-1530`);
    - WP-3: `importFbx`'s warning loop (`:1145-1155`);
    - WP-1: `writeSceneSource` (`:8375-8510`);
    - WP-6: `importFbxModels` (`:1271`).
  - `editor/Project.h` and `editor/Stream.cpp`: WP-2 (Android fields) and WP-6 (build fields) add
    fields, append-only.
  - `docs/mcp/TOOLS.md`, `tools/mcp/budget.json`, `tools/mcp/eval/cases.py`: generated or ratcheted.
    Each package regenerates after merging the previous one. `cases.py` is append-only with distinct
    case ids. Budget lowering is done by the last package to land, from the merged tree.
- **Generated files touched:**
  - `docs/mcp/TOOLS.md`;
  - `editor/resources/templates/` (WP-3);
  - `engine/core/serialization/RuntimeFields.gen.inc`, if REQ-52 adds a cooker field;
  - the Lua stub fragments (WP-3).

## 9. Risks & defaults

- **Cooker parity becomes load-bearing for every build (D1).**
  - Default: AC2 and the AllComponents round trip gate WP-1.
  - A component the cooker loses is fixed in the cooker, never routed back to generated code.
  - If a C++-only feature cannot be expressed as data (none is known: §3.2), it moves to
    `ProjectHooks`, and the spec's Deviations record it.
- **Startup time on Android with huge scenes.**
  - Default: REQ-39's ratio is measured first.
  - If cooked loading is slower than 1.25 ×, the loader is optimised (one allocation pass,
    string-table reuse) before the switch, not given up.
- **Referenced Only drops an asset a script needs.**
  - Default: string literals count, Development Builds name the setting at the failure, and old
    projects default to Ship All Assets.
- **Writing `ShaderWarmup.json` at Stop is still a write (D5).**
  - Default: only on a new key, sorted, so repeated Play sessions leave it unchanged (AC27).
  - Teams that commit it get one meaningful diff per new variant.
- **Opening without listing (REQ-28) surprises a user who relied on it.**
  - Default: Build Settings shows the scene greyed with Add Open Scenes, and `edit_build_scenes add`
    is named in `open_scene`'s description.
  - `create_scene` keeps adding.
- **Shared gizmo renderers change picking.** Default: icon picking moves to screen-space rectangles
  per object (CPU, cheap); AC24 (c) guards it.
- **aapt2's asset filtering is unknown (REQ-13).** Default: AC12 settles it. The content check fails
  the build either way, so nothing ships silently.
- **The Android image grows by about 2 GB (REQ-17).** Default: accepted. Every container otherwise
  downloads the same 2 GB.
- **REQ-31's cause is unmeasured.** Default: profile first. The clipper and the skip of undrawn Scene
  views are taken regardless, since both are free when not needed.
- **Follow-ups (recorded, not done here):**
  - **F1**: a `.mesh` asset referenced by path, so custom meshes leave scene files (E1, with S53).
  - **F2**: install-time asset packs in source-exported Gradle projects.

## 10. Work packages

Six packages, each in its own worktree and branch off `feature/s98-editor-tooling-gaps`, merged
back one at a time. Order: WP-3, WP-2, WP-4, WP-5, WP-6, WP-1. Small and independent first;
WP-1 is the largest and lands last on a tree where everything else is green. Each package runs
`tools/check-changed` and the targeted suites named in its ACs (no full suites, `JOBS=6`).

| WP | Name | Items / REQs | Owns | Size | Depends on |
|---|---|---|---|---|---|
| WP-1 | Scenes as data in every build | E23, E25, E27, E46, E47 / REQ-1-7, 37, 39, 40, 51, 52 | `engine/player/**` (the `runGame` move), `editor/Generator.cpp`, `editor/Factory*.cpp` (deletions), `editor/cook/SceneCooker.*`, `engine/core/serialization/CookedScene.*`, `engine/CMakeLists.txt` project block, `engine/project/**`, Exporter `loadAndSaveAllScenes`/`copyGenerated`/`collectDesktopArtifacts`, `Project.cpp` `writeSceneSource` block, `tools/mcp/tests/test_build*.py`, `test_export_generated_sources.py`, `docs/shipping.md` | XL | none; lands last |
| WP-2 | Android and iOS packages | E4, E28-E32 / REQ-13-20, 38, 49 | `engine/workspaces/androidstudio/**`, `engine/platform/apple/ios/player-template/**`, `editor/platform/AndroidPackager.cpp`, `IosProject.cpp`, `PlatformRules.cpp` (plist helper), Exporter Android/iOS sections, `editor/cli/CommandLine.cpp` export usage, `docker/android.Dockerfile`, `tools/ci/check_android_image.sh`, Android settings rows, `test_android_package.py`, `editor/tests/export/Android*`, `editor/tests/platform/IosTemplate*` | L | none |
| WP-3 | MCP tool fixes and import noise | E2, E17/E43, E34, E37, E44, E45, E38, E39, E40 / REQ-21-26, 34-36, 42, 46 (its cases), 48 | `PrefabTools.cpp` (`create_prefab`), `AssetTools.cpp` (`check_shader`), `PlayTools.cpp` (`wait`, `eval_lua` text), `WorldTools.cpp` (`createParticlesTool` only), `ProfilerClassesLua.cpp`, `SokolSystem.cpp` lease call site, Lua stubs, `editor/util/GlbBuilder.cpp`, `FbxImporter.*`, `ModelImportSettings.*`, the Model Importer Inspector, `set_model_import_settings`/`inspect_models` handlers, `Project.cpp` `importFbx` warning loop, project creation (`McpLaunch.cpp` create path, hub), `tools/templates/make_project_templates.py` and `editor/resources/templates/` (generated), `docs/mcp/guide/prefabs.md`, `profiling.md`, tests `test_prefabs.py`, `test_call_cli.py`, `test_play.py`, `test_particles_tools.py`, `ProfilerLua*`, `FbxImporterWarnings*`, `test_meshes.py` (append), `test_asset_stripping.py` | L | none; first |
| WP-4 | The editor leaves sources alone | E3, E8, E18 / REQ-27-29, 45 (Build Settings part) | `Project.cpp` Stop path and open listing, `ProjectShaderLists.cpp`, `engine/core/.../ShaderWarmup*` (variants), `ProjectPrefab*.cpp`, `StreamPrefab.cpp`, `editor/reload/handlers/Materials.cpp`, `SceneTools.cpp` (`open_scene`), Build Settings window, tests `test_play.py::test_stop_writes_no_scene` (new function, append), `test_project_save.py`, `ProjectPrefabMaterials*` | L | none |
| WP-5 | Editor cost and imports | E11, E12/E42, E15, E21 / REQ-30-33, 41 | `editor/render/SceneRender*.cpp`, `HierarchyWindow.cpp` row loop, `Project.cpp` FBX import scheduling (`:1160-1530`), `ModelLoadCmd.cpp`, `MeshSystem.cpp` load-failure logging, `editor_status` import fields (`CoreTools.cpp`), the FBX delay test hook, tests `test_render_perf.py`, `test_fbx.py` | L | none (coordinate the Hierarchy with S97) |
| WP-6 | What ships | E24, E26 / REQ-8-12, 43, 44 (stripping rows), 45 (report part), 50 (build keys) | Exporter `copyAssets`/`copyCustomShaderSources`, `ExporterPrebuilt.cpp` content staging, `editor/cook/BuildReport.*`, `Project.cpp` `importFbxModels`, the build settings fields (`Project.h`, `Stream.cpp`, reflection rows), Project window context menu, the player's missing-asset message (`engine/core/io` content manifest read), `test_asset_stripping.py` | L | none; before WP-1 (WP-1 rebases onto its Exporter changes) |

Avoiding conflicts:
- No two packages own the same function. Shared files are listed in §8 by region.
- Each package lands its `cases.py` additions with unique case ids. It regenerates `TOOLS.md` after
  merging the previous package. It never edits `budget.json` limits except to lower what it cut. The
  final lowering is checked once on the merged tree.
- WP-1 deletes `Factory` emitters. Packages that add a component field (none planned) would add it to
  the cooker, never to `Factory`.

## Deviations

- **F1 brought into scope (WP-7, E1): the `.mesh` asset.** The coordinator moved F1 out of the
  follow-ups. Decisions, each Unity's behaviour unless noted:
  - **Format.** A binary file (`engine/core/model/MeshAsset.h`): magic `JMSH`, u16 major and minor
    (1.0), a string table, then the cooked scenes' codecs (`CookTypes.h`): the vertices' bounds, the
    vertex count, the interleaved vertex buffer, the index buffer, the external buffers and each
    submesh's primitive type, vertex count and attributes. The bytes depend only on the geometry
    (the counts a load gives, not what a frame set), so the same geometry is the same file. Readers
    refuse another major. Materials, shaders, the skin's palette and the component's settings stay
    on the Mesh Renderer, as on Unity's MeshRenderer.
  - **Identity and reference.** `AssetType::Mesh = 35` (the next free value when it landed), a `.meta` sidecar
    GUID (S04), `AssetKind::Mesh`. `MeshComponent::meshAsset` (an `AssetRef`, Inspector label
    "Mesh", saved as `meshAsset` / `meshAssetGuid`, omitted when empty). A mesh showing a file saves
    neither its buffers nor the bounds and vertex counts they give, so a save does not depend on
    whether the file was read (S63).
  - **Runtime.** `MeshSystem::update` reads the file when `meshAsset` differs from what it last read
    (`loadedMeshAsset`; a missing or bad file is logged once per path), before RenderSystem and
    PhysicsSystem run, in the editor, a prebuilt player and a compiled game alike. A generated
    primitive (`MeshSystem::create*`: Shape fields, `set_mesh_geometry`, `Primitive:create*`) lets go
    of the file: the geometry is the mesh's own again and the file is never written by a geometry
    edit of one GameObject.
  - **Saving through it: both, automatic above 4 KB.** Save Mesh As Asset... (the Mesh Renderer's
    menu, `set_mesh_geometry save_as`) writes any mesh of its own. A scene save also moves each mesh
    of its own with more than 4 KB of geometry (`MeshAssetFiles::EXTRACT_ABOVE_BYTES`) to
    `<scene folder>/<scene name>/<GameObject>.mesh` (Unity's per-scene folder, as for lighting data;
    `<assets>/Meshes/<scene name>/` for a scene outside the assets folder). Rejected: always (every
    cube and plane would become a file of 1-2 KB), and Save As only (a sphere's 45 KB base64 line,
    E1's fragile hand edit, would stay). Meshes of the same geometry share one file; a file there
    with the same bytes is used again (an undo past the save finds it), one with other bytes is never
    written over (`<name> 1.mesh`). Prefab instances keep their prefab's mesh, and prefab saves do not
    extract (a prefab names a `.mesh` through Save As or a GameObject that already shows one). An
    extracted file nothing shows any more stays until deleted, as Unity never deletes assets.
  - **Format bump.** Scene 8 -> 9 and prefab 9 -> 10, identity migrations (`StreamMigrations.cpp`,
    fixture `migrations/scene-v8-inline-mesh.scene`, `MeshAssetMigration.*`): an older file's inline
    buffers still read and keep their meaning, but an older editor must refuse a file whose meshes
    name `.mesh` assets rather than show them empty (S44's sprite-sheet precedent). Cooked scenes
    are format 1.6: the reference is a field of the mesh's table and its buffers are not written
    (`ComponentHooks.cpp writeMesh`); compiled scenes emit `mesh.meshAsset = "..."` instead of the
    buffers (`Factory::createMeshComponent`). Exports ship `.mesh` files through `copyAssets`, which
    copies every asset it does not skip.
  - **Editor.** The Mesh field (drop a `.mesh`, pick one, Reset), Save Mesh As Asset... (one undo
    step; undo keeps the file), dropping a `.mesh` in the Hierarchy or the Scene view makes a
    GameObject showing it, a Project window icon, `t:mesh` search, hot reload of a file another
    program changed.
  - **Lua.** `Mesh.meshAsset` (read at once, so `vertexCount` and `getAABB()` answer straight away).
  - **MCP.** Extended, no new tool: `set_mesh_geometry save_as` (alone: the current geometry; with
    `shape`: the new one), `get_asset_info` on a `.mesh` (format, vertices, indices, submeshes,
    attributes, bounds, users in the open scenes), `list_assets type: mesh`, `set_properties`
    `mesh.meshAsset`. Eval case `guard_mesh_save_as_asset`. The core listing was at its budget:
    `set_mesh_geometry`'s text was shortened to fit `save_as`, and `get_asset_info`'s description
    does not list `.mesh` (its result does; the meshes guide names the call).
  - **With WP-6 (asset stripping):** a `.mesh` is a strippable type: Referenced Only ships the ones
    the dependency index reaches (the reflection asset rows, `AssetKind::Mesh`) and leaves out an
    extracted file nothing shows any more (`MeshAssetSaveTest.AssetStrippingShipsTheMeshesTheScenesShow`).
- **WP-3, REQ-25 (E37):** no code change. Measured first (below): the handler takes 0-2 ms whatever
  the scene; a call's time is its wait for the editor's next frame, which grows with the scene for
  every tool (`editor_status` alike). The frame is WP-5's (REQ-41). `test_particles_perf.py` guards
  REQ-42; it passes on the code before WP-3 too (no red run exists).
- **WP-3, REQ-35:** Material Creation Mode applies to FBX models. A glTF carries the materials it
  draws with and its cook keeps them (S53 R9), so `set_model_import_settings` refuses the key for a
  glTF naming why, and the Inspector row is disabled for one.
- **WP-3, REQ-34:** the summary counts tiling/offset per texture ("on 2 textures"): the importer
  reports it per texture, which is what the user fixes.
- **WP-3, AC33:** `ProfilerLuaGpu.GpuTimingArmsTimers` is in `engine/tests/core/render/`: only the
  core test tree has a headless GPU device (docs/testing.md, "Where a test goes").
- **WP-3, AC19/AC20:** the tests are `test_prefab.py::test_create_prefab_overwrite` (the existing
  prefab file) and a new `test_shaders.py`.
- **WP-3, REQ-36:** the MCP test harness keeps the flat layout for the projects it creates
  (`JASPER_TEST_FLAT_NEW_PROJECT=1`, docs/testing.md): the suite's tests address assets at the
  project root. `test_project_create.py` and the template generator use the real layout.
- **WP-2: F2 brought into scope (coordinator decision).** Source exports get the install-time asset
  pack too: the template has a `game_content` module (`com.android.asset-pack`, install-time) and
  `assetPacks = [":game_content"]`. Android puts asset packs in App Bundles only (an APK from
  `assemble` carries none), so the content goes to the pack when a Gradle run builds bundles and no
  APK (`bundleRelease`, Android Studio's APK from app bundle), decided from the task graph; a run that
  builds an APK too keeps it in the base module of both and logs why. D11's "source exports keep one
  module" no longer holds.
- **WP-2, REQ-13: aapt2 applies no `<dir>_*` default.** `aapt2 link -A` (build-tools 36.0.0) keeps
  `_Onboarding/` and drops only hidden files; it has no `--ignore-assets` option. So nothing is
  passed to it; instead the prebuilt staging leaves out the names the template's pattern leaves out
  (hidden and VCS files), with `W-ANDROID-ASSET-IGNORED` naming them, and the content check compares
  every staged file with the finished package. The source route checks the same in Gradle
  (`jasperengineVerifyContent` after `assemble*`/`bundle*`).
- **WP-2, AC12 and AC14 run in `tools/ci/android_packaging.sh`** (the S67 route), not in an MCP
  `test_android_package.py`: the MCP suite runs in the Linux image, which has no Android SDK,
  bundletool or emulator. The script's step 3b checks the `_` folder, the hidden file, the drop hook
  (`JASPER_TEST_ANDROID_DROP_ENTRY`), the pack's `dist:install-time`, the universal APK's content and
  the suffix; step 4 runs the bundle on the emulator; `ANDROID_PACKAGING_SOURCE=1` checks the source
  export's pack, suffix and `-O2`.
- **WP-2, REQ-14: the debug type is a Development Build.** Besides `RelWithDebInfo` it passes
  `JASPERENGINE_DEVELOPMENT_BUILD=ON` and `JASPERENGINE_HTMLUI_DEBUGGER=ON`: AGP's `Debug` config was
  what made a debug APK (and the development Android player, built with `assembleDebug`) a
  Development Build, so `RelWithDebInfo` alone would have dropped the profiler from both.
- **WP-2, REQ-17:** the pinned CMake is 3.22.1, the version AGP 9.4.1 installed by default, so the
  build's CMake does not change.
- **WP-2, REQ-19:** with only the adaptive layers set, the legacy and round icons are composed from
  them (the 72 dp a launcher's mask shows), so no template icon ships for Android 7 and older.
- **WP-2, REQ-44:** the Android tab's Apply is one undo step (`AndroidSettingsCmd`) over every
  Android row, Debug Package Suffix included; `set_project_settings` stays not undoable, as for
  every row.

- **WP-1, REQ-1 (D1 sketch):** `player::runGame(const PlayerConfig&, const ProjectHooks&, contentUrl)`
  (`engine/core/player/GameRunner.h`). The hooks are the C++ scripts' entry points scene_scripts.cpp
  already had: `initScripts`/`cleanupScripts` per scene and the three ObjectManager hooks (instantiated,
  destroyed, moved GameObjects); no Lua-bindings hook is needed (a C++ script registers nothing in Lua).
  A Compile build reads `game.json` as the prebuilt player does, so every setting its generated
  `init()` used to set moved into game.json and `player::applySettings`: Shader Creation, the Canvas
  Scaler default, GPU Instancing, Shader Globals, the 3D world capacity, Player > Resolution and
  Presentation, Allow Insecure HTTP, the application info, Project Settings > Debug, the terrain
  tiers, native plugins per target. The prebuilt player applied none of those before: it now does.
- **WP-1, REQ-1 (layout):** an exported project holds the cooked content under `project/content/`
  (`game.json`, `scenes/`, `prefabs/`), which each platform's build puts at the game's content root
  beside `assets/` and `lua/`: the exported CMake (desktop copy and install, the Apple bundle, the
  web's `--preload-file content@/`), the Android template's content sync (WP-2's `jasperengineSync*`), and the exported Xcode
  project's resources (`Exporter::addXcodeProjectContent`). A built game's folder has the prebuilt
  layout (`game.json`, `scenes/`, `prefabs/` next to `assets/`).
- **WP-1, REQ-1 (deletions):** with Factory's emitters went the reflection tables' exported-code
  hooks (`codegen*`, `cppLiteral`, `cppStatements`, the NoCodegen/CodegenIfChanged/CodegenRuntime
  flags, `reflect/Codegen.cpp`), the navigation and voxel settings' code, the curve literals and the
  scene-dump `bake` op; `toIdentifier` moved to `editor/util/CppIdentifier.h`, `BakedPrefab` to
  `cook/SceneCooker.h`. The tests that pinned generated code were removed or turned into cooked
  round trips. WP-7's `mesh.meshAsset` line in `Factory::createMeshComponent` went with it: a
  Compile build reads the reference from the cooked mesh table, as the prebuilt player does.
- **WP-1, REQ-52:** the cooker dropped four of a scene's preload lists (VATs, masks, controllers,
  bitmap fonts) that the generated `resources_<scene>()` had: cooked format 1.7 carries them (an older
  file still reads; 1.6 is WP-7's .mesh asset).
- **WP-1, REQ-2:** the project's own CMake build reads `<internal>/cooked/` through
  `JASPERENGINE_COOKED_PATH`; its asset path stays the project's assets folder, so a scene drawing
  the editor's built-in default sky (faces held in memory by the editor) has no sky there. Exports
  ship the faces as before.
- **WP-1, REQ-4:** `W-SCENE-UNSAVED` replaces `W-SCENE-NOT-SAVED` (one code for a never-saved scene,
  a modified one and a modified Prefab Mode stage). A build cooks each scene as its file would hold
  it now (encoded and decoded into a scratch scene, `Project::withAuthoredScene`), so what drawing or
  loading made of it in the editor (a camera sized to the view, a Text's atlas, loaded geometry) is
  no part of the build (REQ-5); a scene whose save would lose a GameObject stops the build as it
  stopped the save-first export. Terrain maps still reach their files on an export's flush: they
  persist on their own a second after an edit (T1 R29), which is no unsaved change of the user's.
- **WP-1, REQ-52 (hierarchy):** a cooked scene carried no hierarchy (a Transform's parent is no
  field of its record; only prefabs had a hierarchy record), so a prebuilt player made every child
  a root. Format 1.7 adds the scene's hierarchy record after the resources (an older player skips
  it). A texture with no image but a sampler (a Text's font atlas, a sprite sheet's page) is
  written as an empty id, which every 1.x reader takes, instead of nothing.
- **WP-1, REQ-5 / E27:** the determinism guard is `CookDeterminism.EveryComponentDefaultInitialisedCooksTheSameBytes`
  (every component default-initialised over two fillings cooks the same bytes; `ComponentDesc::defaultInitialise`
  added for it) plus AC6.
- **WP-1, REQ-10 in Compile builds:** `player::readGameConfig` (the generated main's) applies the
  `stripped` list of a `content-manifest.json` beside `game.json`, as the prebuilt player does.

- **WP-5, AC24 (a):** the AC24 fixture logs no "No free system entity slot" on the code before
  WP-5 (its gizmos took about 450 of the 1,000 system slots), so `test_editor_cost.py` guards it
  and the red run is `SceneRenderGizmos.SystemEntitiesPerKind` (500 lights and 500 bodies: the
  slot error, then an exception, before WP-5; at most 3 system entities after). The test is in
  `editor/tests/render/`, which the AC names.
- **WP-5, REQ-30:** the icons are one instanced billboard sprite over an atlas of the five icon
  images (one icon batch per view, as specified); the lines are one `LineRenderer` per kind, in
  the scene for depth-tested kinds and in the tools layer for the ones drawn over the meshes, as
  before. A Game view pass that hid the batches makes them write their renderer again when they
  show (no stale instance list).
- **WP-5, REQ-31:** Maximize On Play covers the editor's panels: the covered Hierarchy is not
  built and the covered Scene view updates and draws nothing (Unity hides them too).
- **WP-5, REQ-31 (Hierarchy):** the recurring timed rebuild meets the 1 ms budget (0.62-0.69 ms with
  the 1,500-GameObject fixture, enforced), but a structural change (a command that changes the tree)
  still rebuilds the whole Hierarchy tree once per edit: 1.6-1.9 ms with that
  fixture, measured with phase scopes. A per-edit incremental patch of the tree was not done: the
  cost is one rebuild per user edit, not per frame.
- **WP-4, REQ-45:** Add Open Scenes in Build Settings is not undoable, like the rest of the Scenes
  In Build list it edits (`edit_build_scenes` is not either). Closing a scene opened out of the build
  takes it out of the editor (it is in no list to reopen from but the workspace).
- **WP-4, REQ-27:** builds read `ShaderWarmup.json`'s variants whatever Include in Builds says (the
  variants a Play session found used to live in the scene files, which builds always read).
- **WP-2, REQ-13/AC12:** a source-exported Android package draws the golden on the emulator but
  still fails the GLES log check on S67's open `GL_FRAMEBUFFER_STATUS_INCOMPLETE_MULTISAMPLE`; not an
  S98 change.
- **Integration, REQ-44 (Material Creation Mode in settings search):** settings search lists
  Project Settings and Preferences rows, and Material Creation Mode was a per-model importer setting
  only. It became also a project default, as the Mesh page already holds one for every other model
  setting: Project Settings > Mesh > Material Creation Mode (`mesh.material_creation_mode`, stored as
  `mesh.materialCreationMode`, written only when None; undoable with the page's Apply) is the mode of
  the FBX models whose sidecar sets none; a model's own value wins. A change makes those imports
  stale and their cooks import again. The Model Importer and `inspect_models` show the resolved
  mode. Tests: `SettingsSearch.FindsTheS98Settings`,
  `FbxImporterWarnings.ProjectMaterialCreationModeIsTheDefault`,
  `test_meshes.py::test_project_material_creation_mode`.
- **Coordinator decisions carried here:** F1 (`.mesh` assets, WP-7) and F2 (install-time asset packs
  for source exports, WP-2) moved into scope; E35 went to S94 (R45), E36 to S96 (WP-C), E41's
  `mesh.sortPriority` to S94 (R48).
- **Integration, fixes of `main`'s failures:** WP-3 fixed the CMake 4.5 configure error
  (`install(CODE ... CONFIGURATIONS)`), the Lua API extractor's unresolved Grid/TitleArt bindings and
  `ShaderNeutralApi`'s TitleArt check, which the other program PRs list as fixed by S98. `main` has
  since fixed all three itself (`f1d3e14dc`, `6f377bf5f`), and also
  `ContentManifest.RefusesContentNeedingAModuleThePlayerLacks`' module list, the
  `test_scene_view_icons.py` camera-gauge flake (`f88fbdadb`) and `test_bake_vertex_animation.py`'s
  two failures (`18181a541`). The merge of `main` took `main`'s versions; S98 carries none of them now.
- **Across PRs (integration):**
  - The cooked scene hierarchy record (WP-1, REQ-52) fixes the bug S96's PR #8 fixes in
    `c633f787f`, with the same record (a (child, parent) list after the resources). Whichever PR
    merges second keeps the one already on `main` and drops its own writer and reader.
  - Format numbers: S98 takes scene 9 / prefab 10 (WP-7) and cooked 1.6 (WP-7) / 1.7 (WP-1), the
    next above `main`'s 8 / 9 / 1.5. S94 also bumps scene 9 / prefab 10, and S92, S95 and S96 bump
    the cooked minor to 1.6: the PR merged second renumbers and chains its migration.
- **Integration, MCP harness:** `harness.compile_env` emptied its compile home as a test file loaded;
  the runner's `--list` (every run's `-k`) loads every file, so a second run started next to one
  compiling wiped its compile tree mid-build (AC8's one failure). The home is now emptied when the
  process starts its first editor with it.
- **Integration, AC6 red control:** with `solidBoxDensity`'s initialiser removed (editor and
  runtime built that way), `test_export_sources.py::test_export_is_deterministic` still passes: two
  exports read the same bytes for the uninitialised float on this allocator, so an export-level
  comparison cannot show the bug. The guard of E27 is the unit test
  `CookDeterminism.EveryComponentDefaultInitialisedCooksTheSameBytes`, which fails naming
  `RigidbodyComponent` with the same initialiser removed (WP-1's red run); the MCP test stays as the
  end-to-end check of REQ-5's other sources of drift (stamps, ordering). AC3's red control holds: with
  the C++ script hooks set aside, the exported player logs none of the `parity cpp` lines.
- **Integration, Android x86_64 (a failure of `main`):** a fresh Android x86_64 build failed in
  dav1d's NASM sources (`unable to open include file 'thread'`) on `main` as on this branch: CMake
  3.22's FindThreads test fails on bionic (no `pthread_cancel`), falls back to `-pthread`, and
  `Threads::Threads` hands it to every language; NASM reads it as `-p thread`. dav1d now links the
  thread libraries and adds `-pthread` for C only (`engine/libs/dav1d/CMakeLists.txt`, its
  README.jasper.md). Needed to build REQ-1's Android source export for the integration check.
- **Integration, REQ-11 (a fork named relative to the assets directory):** the editor finds a
  `customShader` relative to the project root, else to the assets directory (`Project::findShaderFile`).
  REQ-11 shipped each source once, at its project-relative path, so a fork named the second way
  (`shaders/globals_color` for `assets/shaders/globals_color.frag`) had no manifest in a build and
  drew with zero-initialised parameters (`test_shader_globals.py::test_desktop_export_declares_the_globals`,
  red on the merged tree). `copyCustomShaderSources` now also ships the entry points of every
  referenced fork that resolves only that way at the name it is referenced by: such a fork has two
  copies, an unreferenced or project-relative one still one.
- **Integration, Android emulator (open):** `tools/ci/android_packaging.sh` on the merged tree passes
  every packaging, signing, bundletool and content check, and the source export (cooked content under
  `project/content`, no scene sources) builds with Gradle into a signed APK and bundle. On the
  emulator every package draws the golden (0.07 %), but the prebuilt development APK now fails the
  GLES log check with S67's open `GL_FRAMEBUFFER_STATUS_INCOMPLETE_MULTISAMPLE` (after the emulator's
  `depth attachment not complete`), as source exports always did. The tree before the merge of `main`
  fails the same way, and WP-2's run before WP-1 did not: since REQ-10 the prebuilt player applies
  every setting a compiled game applied (the Debug Overlay among them), so both routes now meet that
  error (inferred from the list of settings the player applies; not bisected further). Whether a
  device reports it is still unknown (S67).

### WP-6 (what ships)

- **Keys under `export:`.** The settings are stored as `export.assetStripping` (`referencedOnly`;
  absent = Ship All Assets), `export.alwaysInclude` and `export.exclude`, beside the other Player >
  Publishing keys (`method`, `pack`, `stripEngineCode`); `project.yaml` has no `build:` map. The MCP
  rows are `player.asset_stripping` (`ship_all` / `referenced_only`), `player.always_include` and
  `player.exclude_from_build`.
- **What Referenced Only may leave out.** Only content file types: textures, models, audio, video,
  fonts, materials, prefabs, UI documents and stylesheets, colour LUTs, particle effects, animation
  assets (clips, pose and vertex animations, controllers, masks, montages), Aseprite files,
  tilesets, Sound Events, Title Art and voxel volumes. Lua (REQ-9), shader sources (REQ-11), scenes,
  data files, palettes, navigation data, terrain worlds and other files always ship: the runtime
  reads several of them by convention or through dependencies the index does not hold.
- **More literal sources.** Besides Lua and C++ sources, the scalars of `project.yaml` and the
  string literals of shipped `.json` data files count: the window icon and the audio mixer are named
  there and are not index edges; a level file listing its sprites is the same case as a script.
  A kept bitmap font keeps its sheets and TrueType source, and a kept FBX keeps the textures its
  import links by URI (neither is an index edge).
- **Exclude From Build everywhere.** It also leaves out Lua files (`copyLua`) and shader sources, and
  `assetmap.bin` leaves out every left-out asset (so `Asset.exists` answers false for one).
- **The Development Build message** is a warning from `File::open` (once per path), before the
  loader's own "not found" line, which stays. Prebuilt players read the list at start; a Compile
  build reads `content-manifest.json` once WP-1 runs it through `player::runGame` (REQ-1), and a
  source export has no manifest.
- **New projects** get Referenced Only in `createProjectInDirectory` (templates take it from there);
  `JASPER_TEST_FLAT_NEW_PROJECT=1` (WP-3's harness default) keeps the projects the MCP suite creates
  as they were before S98.
- **AC10's test** is `test_asset_stripping.py::test_shader_shipped_once` (`test_export_sources.py`
  is WP-1's file) and checks the copies; the exported player drawing it is not run there.
- **AC9's player check** is `test_asset_stripping_player.py` (slow, prebuilt Development runtime),
  apart from the CI file so that file waits for no runtime.

### The main merge before landing (lead's review)

- **Older prefabs keep their embedded materials (REQ-29).** The lead's rule is that old content
  behaves as before. Prefab 9 -> 10 (`StreamMigrations` `prefabV9ToV10`, beside the mesh-asset
  identity) writes `followMaterialFiles: false` into a version 9 prefab with a submesh linked to a
  `.material`: its asset, Prefab Mode, Play's templates, the cook and the export keep the copies it
  embeds, as on main; its saves keep the key. A prefab without a link, and every new one, follows its
  files. Instances in scenes are linked at scene open as on main. Fixture
  `editor/tests/fixtures/migrations/prefab-v9-linked-material.prefab`, test
  `ProjectPrefabMaterials.AnOlderPrefabKeepsItsEmbeddedCopies`.
- **The save's mesh extraction is an undo step.** `MeshAssetFiles::extractScene` shows the written
  files through one step of the scene's history, "Save Meshes As Assets" (a `CommandGroup` of
  `PropertyCmd<AssetRef>` on `meshAsset`, inserted with `insertCommand`, so the redo tail stays).
  Undone, the GameObjects hold their geometry in the scene again; the files stay.
  `MeshAssetSaveTest.TheSaveShowsTheFilesThroughAnUndoStep`.
- **What the generated startup code applied, inventoried.** Every call of main's generated `init()`
  (`Generator::configure`) has a `game.json` field that `player::applySettings` applies; window and
  VSync settings still reach a Compile build through the generated CMake config. Of what
  `Factory::createScene` set on a scene, the cooked settings record lacked the scene's Canvas Scaler,
  its Use Project Default and Physics 2D Sub Steps (prebuilt builds on main lost them too): they are
  appended to the record in format 1.7 (`CookedScene.SceneSettingsKeepCanvasScalerAndSubSteps`).
  `test_build_parity.py` runs both builds of one game and compares what a Lua probe prints of the
  changed settings (slow: JASPER_SLOW_BUILD=1).
- **Fewer MCP files.** The new CI files moved into existing ones, as AGENTS.md asks:
  `test_shaders.py` into `test_call_cli.py`, `test_mesh_assets.py` into `test_meshes.py`,
  `test_import_blocking.py` into `test_fbx.py`, `test_build_scenes.py` into `test_project_save.py`,
  `test_particles_perf.py` into `test_particles_tools.py`, `test_hierarchy_refresh.py` into
  `test_project_window_open.py`, `test_editor_cost.py` into `test_render_perf.py`,
  `test_export_sources.py` into `test_export_generated_sources.py`, and `test_project_create.py` and
  `test_asset_stripping_player.py` into `test_asset_stripping.py`. That file stays new: it needs a
  project with a new project's real layout (`JASPER_TEST_FLAT_NEW_PROJECT=0`), which no existing
  file's tests are written for. `test_build_parity.py` stays new and out of the CI suite.
- **Numbers taken in merge order.** Landing right after S96 (#8), S98 takes the next free values
  then: cooked format 1.7 (S96 has 1.6: its physics world settings and its scene hierarchy record,
  whose writer and reader S98 keeps instead of its own), scene 9, prefab 10 and `AssetType::Mesh =
  35` (it had been 37, beside S95's reserved 35 and 36; the lead's rule gives a stored number to the
  PR that lands first, and the branches after it renumber).
- **`JASPER_TEST_FLAT_NEW_PROJECT` stays, as a test hook.** It follows main's `JASPER_TEST_*`
  hooks (about fifteen, e.g. `JASPER_TEST_EXPORT_HOLD`, `JASPER_TEST_SCENE_SAVE_DELAY_MS`). It only
  picks the layout `createProjectInDirectory` gives a new folder, and the flat layout it keeps is a
  real one: every project from before S98, and any `project.yaml` without the keys. Removing it would
  rewrite the asset paths of most of the MCP suite. The real layout is tested by
  `test_asset_stripping.py` and the template generator.

## Progress

- **WP-3, E37 measurement** (unchanged editor, headless, llvmpipe; `JASPER_MCP_TRACE=1`):
  `create_particles` median round trip 170 ms in an empty scene, 431 ms with 1,000 cubes, 610 ms
  with 2,000; the editor's median frame 18, 431 and 618 ms; `editor_status` 207, 354 and 634 ms.
  Trace of 45 calls: queue wait 105-1237 ms, handler 0-2 ms. REQ-42's fixture (300 calls into
  1,000 GameObjects): first 50 mean 202 ms, last 50 mean 130 ms, ratio 0.64.
- **WP-1, REQ-39 measurement** (before the switch, one run of one Release binary, dev host, Linux
  x86_64 GCC 13 -O3): the REQ-39 fixture (2,000 GameObjects made through MCP: 700 cubes, 400
  sprites, 200 point lights, 300 bodies, 250 Lua and 149 C++ scripts) exported as source by the
  editor before WP-1 (generated `scene_New_Scene_*.cpp`: 172,675 lines, 6.8 MB), with a timing harness
  in its `main.cpp` building the scene 11 times each way, alternated, from the generated
  `create_<scene>()` and from the same scene cooked by this editor (5.2 MB `.jscn`): median 51.9 ms
  generated, 44.0 ms cooked, ratio 0.85 (REQ-39 allows 1.25). Compiling that one generated file
  took 607 s (`.ninja_log`) of the export's build, per ABI; it is gone. After the switch,
  `CookedSceneBench.LoadsTwoThousandGameObjects` holds the 60 ms budget (perfSlack).
- **WP-1, Android** (local KVM host, `jasperengine/android-build`, Gradle `assembleDebug`, one ABI
  arm64-v8a, the worktree's Android Studio template on a scratch project): the same generated
  REQ-39 scene file took 52.2 s of compile (`.ninja_log`) in a Debug build; a cooked build compiles
  no scene source. AC7 manual: after that build, the scene file was removed and a new
  `probe_configure_depends.cpp` added; the next `assembleDebug` printed `GLOB mismatch!`, compiled
  the probe, dropped the scene from `build.ninja`, and linked `app-debug.apk`.
- **WP-7 (F1, `.mesh` assets)** implemented on `feature/s98-editor-tooling-gaps--7` (Deviations).
  Red controls: `test_mesh_assets.py` on the editor of `3ef9338fa` (3 of 3 fail: `save_as` unknown,
  no `.mesh` written); the editor unit tests with `MeshSystem`'s read and the save's extraction
  turned off (5 of 7 `MeshAsset*` fail). Evidence: `/tmp/arcade-gaps/evidence/s98-7/`.
- **WP-5, REQ-31 profile** (`test_editor_cost.py`, its generated fixture: 1,500 GameObjects under 12
  groups, Hierarchy expanded, nothing selected; Release editor, headless, llvmpipe, build box;
  median frame of 60 of `capture_profile`). Before WP-5 (the base with only the new scopes):
  Editor UI 8.09 ms (Hierarchy 6.14, of which its rows 3.07; Inspector 0.74; Scene Window 0.11),
  Scene View Gizmos 0.55 ms: editor-side 8.64 ms. Play with the Game view maximised: Hierarchy
  2.94 + Scene View Gizmos 0.63 + Scene Window 0.08 = 3.65 ms. After: Editor UI 2.40-2.52 ms
  (Hierarchy 0.58-0.72, rows 0.46-0.62; Inspector 0.62-0.85), Scene View Gizmos 0.31-0.41 ms:
  editor-side 2.71-2.93 ms; maximised Play 0.06 ms (only the Scene window's tab).
  - Findings: the Hierarchy drew every row (now only the visible ones); its timed 0.5 s tree
    rebuild (3.8 ms with 1,500 GameObjects) ran on every frame of an editor slower than 2 fps (one
    run measured it at the median, Hierarchy 4.4 ms): it now also waits 15 frames; under Maximize
    On Play the covered Hierarchy was built and the Scene view updated its gizmos each frame (now
    neither). The per-object gizmo objects cost a system entity each and a renderer update each;
    the shared batches write a renderer only when its lines or icons changed.
  - The Hierarchy's tree rebuild (`test_editor_cost.py::test_hierarchy_rebuild_budget`: the median
    of the timed rebuilds in 120 frames, three runs, enforced with `JASPER_MCP_PERF_SLACK=1`): 2.28,
    2.28, 2.41 ms before (red, over the 1 ms); 0.62-0.69 ms after in four runs (0.72 ms in a run
    with `-j 3`). Phases measured first (one rebuild, ms): the row loop 0.6-0.9, the set of the scene's
    GameObjects 0.36-0.48, freeing the old tree 0.23-0.33, the GameObjects without a Transform
    0.16-0.35, the prefab passes 0.13-0.27; in the rows, the locked-parent scan 0.51 and the icon
    chain 0.24 of 1.3. Now the timed rebuild, which catches what changes without a structural
    command, fills the last rebuild's rows again in place while every row keeps its order and
    parent (no allocation), from indexes made once (LockedParentIndex, the icons by signature, the
    model ancestors); a structural change still rebuilds the whole tree, once per change: 1.6-1.9 ms with
    the fixture, measured with phase scopes before the in-place refresh (the full path has since
    only lost the set's rebuild and the rows' copies; not measured again: a capture cannot start
    before the frame a command's rebuild runs in). `LockedParentIndex` now also answers the Voxel Volumes' and
    splines' generated GameObjects, as `getLockedEntityParent` does (it had fallen behind).
  - On the tree merged with WP-3, WP-6 and the imports: 2.07-3.07 ms editor UI in four runs (one
    alone, three in parallel); one run among eight files with `-j 3` measured Hierarchy rows at
    4.6 ms (editor-side 7.0 ms, within the 8 ms), as if no row were clipped. Not reproduced in
    the eight runs since (four alone, four among other files with `-j 3`: 0.41-0.64 ms);
    unexplained.
  - `test_scene_view_icons.py` fails under load (three copies with `-j 3` on a build box) on the
    code before WP-5 as after it, by the same 0.056: the difference is the Scene view's camera
    gauge fading between the shots (pixels x 6-37), not the icon, which is back. Not changed here.
- **WP-5 imports (E15, E21; REQ-32, REQ-33, D10).** `editor/model/FbxImportQueue` is the one
  scheduler: any thread queues an FBX; one Background Heavy job converts the queue once a frame
  queued nothing new (a scene's load is one batch); Play startup, the export (before
  `loadAndSaveAllScenes`, on its thread) and reload jobs convert on their own threads.
  `fbxImportCallMutex` and the scan task are gone. Main-thread callers queue: `ModelLoadCmd`
  waits (`whenModelImported`), foliage layers draw once imported, `import_files` replies from a
  deferred wait, thumbnails ask again, the cook queue keeps the model queued, VAT bake/preview
  report "still importing". `MeshSystem::setModelImportGate` keeps a pending model waiting with
  no error. `editor_status` lists the `model_import` job with `done`/`total`. Red control (the
  delay hook alone on `3ef9338fa`): AC25 fails (`read_console` answered only after
  `create_entity` converted on the main thread), AC26 fails ("Model load failed, not retrying").
  The guide gained no line: the result says it (eval case `guard_model_import_pending`, not run).
- **WP-6, what ships** (AC9, AC10, AC11, AC35, AC36): `test_asset_stripping.py` 6/6 on the WP-6
  editor; red on the spec branch's editor (3ef9338fa): 0/6, everything shipped (`textures/unused.png`
  and `dev/debug.png` cooked), Play imported the unused FBX, two copies of the shader source
  (`assets/assets/shaders/Plain.frag` and `assets/shaders/Plain.frag`), the settings unknown. The
  WP-6 unit tests (`AssetStripping.*`, `AssetStrippingProjectTest.*`, `StreamProjectSettings*.*`,
  `ContentManifest.StrippedListRoundTripsAndIsAbsentWhenEmpty`, `StrippedAssets.*`) use the new API
  and do not build on the old one. AC36: screenshots of Project Settings > Player > Build, the
  Project window's two items and an Exclude From Build undone with Edit > Undo (outside the
  repository, the package's evidence folder).
- **WP-1, verification** (targeted runs; full suites not run). Red controls on a scratch copy with
  each fix set aside: `CookDeterminism.EveryComponentDefaultInitialisedCooksTheSameBytes` fails naming
  `RigidbodyComponent` with `solidBoxDensity`'s initialiser removed (E27/AC6 guard);
  `ProjectCookCacheTest.WritesCookedSceneOnSave` fails with the stale-source removal off (AC4);
  `ExportTemplate.ProjectGlobConfigureDepends` fails without the keyword (AC7);
  `StrippedAssets.ACompileBuildReadsTheListBesideItsGameJson` fails without the manifest read.
  `CookedScenes.ParentsAreBuiltBeforeChildren`, `CookDeterminism.ATextShipsNoFontAtlas` and
  `Stream.SubmeshVertexCountNotTheLastDraws` failed on the first cooked build (the scene hierarchy,
  the sampler and the vertex counts were lost) and pass with the fixes. `test_export_sources.py`
  run on the editor before WP-1: both tests fail (no `W-SCENE-UNSAVED`, the export saved the scenes;
  no cooked scene in a source export). The MCP-level red of AC3 (hooks set aside) and AC6
  (`solidBoxDensity` in an export) were not run. On the tree merged with WP-3 to WP-7:
  `WERROR=1 tools/build.sh linux` and the glcore release player build clean; the targeted CI MCP
  files (export sources/pipeline, build, build scenes, save determinism/completeness, shader lists,
  asset stripping, mesh assets, CLI paths) 73 passed, 4 skipped (the slow ones), 0 failed;
  `test_build_parity` (AC2), `test_desktop_build_and_run` (AC1), `test_compile_build_removes_stale_files`
  (AC8) and `test_lifecycle_export` (AC3) pass with `JASPER_SLOW_BUILD=1`.
- **WP-2, Android end to end** (`ANDROID_PACKAGING_SOURCE=1 tools/ci/android_packaging.sh development`,
  local KVM host, x86_64 development runtime from `tools/build.sh players`): every prebuilt check
  passed, step 3b included: `_Onboarding/level.json` in the APK and in `game_content` of the bundle,
  `.gitkeep` left out with `W-ANDROID-ASSET-IGNORED`, package `com.jasperengine.packaging.dbg`,
  `bundletool dump manifest --module game_content` shows `dist:install-time`, the universal APK has
  all 62 content files, and `JASPER_TEST_ANDROID_DROP_ENTRY` fails the APK and the bundle naming the
  entry. The emulator ran the prebuilt APK and the bundle (install-time pack) to the golden (0.07 %).
  The source export (`export --platform android`: GLES3 shaders only) built with Gradle: the APK, then
  the bundle in a run of its own with the content in `game_content`; its debug native code is `-O2`
  (2,005 compile commands, no `-O0`, `JASPERENGINE_DEVELOPMENT_BUILD=1`). On the emulator both source
  packages draw the golden (0.00 %) and still fail the GLES log check with S67's open
  `GL_FRAMEBUFFER_STATUS_INCOMPLETE_MULTISAMPLE`; one bundle run also met an ANR dialog on the loaded
  host (frames of 2.4-6.6 s), and its rerun drew the golden. After merging WP-3 to WP-7 (a new
  runtime, the prebuilt part only): every check passed again; the bundle's first emulator run met the
  same ANR dialog, its rerun drew the golden (0.07 %).
- **WP-2, Gradle template without native code** (`./gradlew ... -x configureCMake* -x buildCMake*` in
  the Android image): `bundleDebug` puts the content in `game_content`, `assembleDebug` in the APK,
  both in one run keep it in `base` and say so; a hidden file is left out and named. Red control:
  with `ignoreAssetsPattern` removed, AGP drops `_Onboarding/` and the build fails with
  "app-debug.apk lacks 1 file(s) of the game's content (assets/): _Onboarding/a.txt".
- **WP-2, image:** the rebuilt `docker/android.Dockerfile` image ran the player and game builds with
  no NDK or CMake download (Gradle's log has no "Installing"). `tools/ci/check_android_image.sh`
  passes in it (`ndk;28.2.13676358`, `cmake;3.22.1`) and fails in the image before (red: neither is
  installed).
- **WP-2, unit tests** (`UNIT_SUITE=editor`, filter `IosTemplate.*:AndroidTemplate.*:AndroidIcons.*:
  AndroidPackage.*:AndroidSigning.*:BuildMethod.*:IosBuild.*:PayloadValidator.*:SettingsRegistry.*:
  SettingsSearch.*:PlatformBackends.*:PlatformServices.*:ExportTreeWalk.*:*Android*:*Ios*`, WERROR):
  71 ran, 0 failed, 0 skipped, 0 disabled, before and after merging WP-3 to WP-7. Red control (the
  templates, Dockerfile and Info.plist of `3ef9338fa`, and the packager's check, filter, suffix and
  icons, the export's debug type, signing and icons, and the prebuilt Info.plist rewrite turned
  off): exactly the 8 new behaviour tests fail (`AndroidIcons.WritesEveryDensityAndRound`,
  `AndroidTemplate.IgnoreAssetsPatternKeepsUnderscoreDirs`, `.DebugTypeBuildsOptimisedNative`,
  `.ReleaseSigningFromKeystore`, `.PinsTheImagesNdkAndCMake`, `.BundlesCarryTheInstallTimeAssetPack`,
  `AndroidPackage.EveryContentFileIsInThePackage`, `IosTemplate.OrientationsFromPresentation`).
- **WP-2, editor UI:** Project Settings > Player > Android > Debug Package Suffix found by the
  settings search, `.dbg` applied with OK into `project.yaml`, and undone as one step ("Change
  Android Settings", `undo scope: timeline`); screenshot in `/tmp/arcade-gaps/evidence/s98-2/`.
- **WP-2, `export --platform`:** `test_call_cli.py::test_export_platform_android` passes (the file:
  13 passed, 1 skipped by design); red: an editor without the change (s98-4's build) exits with
  "Unknown argument: --platform".
