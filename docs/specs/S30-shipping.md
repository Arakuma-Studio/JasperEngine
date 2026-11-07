# S30 — Shipping: prebuilt-player export, headless player, publish/verify pipeline

Wave 3. Depends on S02 (CI, Docker images, browser gate, `check-notices.py`), S03 (atomic publish),
S04 (`AssetMap`), S06 (web thread pool), S08 (reflection tables) and S14 (texture cooks).

## 1. Goal

An artist with no compiler can build a game for any of our six targets. The editor stages cooked
content (scenes, Lua, UI, textures and shaders) next to a **prebuilt player runtime** that CI built
for that target, backend and flavour. Only projects that really need native code (C++ scripts,
`ProjectBuild.cmake`, or capacities larger than the player's) still go through today's CMake build.
Every player also gets deterministic headless run flags, so CI and agents can run a shipped build
and take a screenshot of it. A publish pipeline then refuses to ship broken releases: wrong
architectures, case collisions, symlink escapes, missing content, content changed after prepare,
or missing licence notices. It splits off debug symbols, writes SteamPipe scripts and deploys to a
Steam Deck.

## 2. Scope

**In scope**
- The `jasperengine-player` runtime: it reads its configuration, cooked scenes and content at
  runtime.
- The cooked scene format and its runtime loader.
- The CI matrix that builds players, the player archives, and player install/download in the
  editor.
- The prebuilt build method in `Exporter`, for every target:
  - desktop: staging plus re-signing or resource patching;
  - Android: APK/AAB repackaged without the NDK, CMake or Gradle;
  - iOS: a prebuilt-library Xcode project;
  - web: a wasm template with a `.jpak` content pack and the COI service worker.
- Player launch options: `--headless`, `--quit-after`, `--fixed-fps`, `--screenshot` and related
  flags.
- `publish` (prepare, verify, Steam VDF and upload, Deck deploy and rollback) as Build Settings UX,
  CLI commands and MCP tools.
- Third-party notices staged into every shipped build.
- Smoke-test and CI jobs that need no compiler.

**Out of scope**
- The C#/CoreCLR runtime, NativeAOT and packages checks in `publish_game.py` (README do-not-copy
  list).
- Their WebGPU/WGSL cook, WebEditor and npm package (do-not-copy, report 10 §6). Web stays on
  WebGL2.
- A Steamworks SDK integration (achievements, overlay). Notarization and App Store / Play Store
  upload APIs (documented as manual steps).
- A device-less "no GPU" run mode: `--headless` still creates a graphics device (D9).
- AppImage/dmg packaging for exported games (the editor's own packaging is unchanged).
- Hot reload in players (S05). The crash handler in players (S28 owns it; players link it).

## 3. Current state

- **Every build compiles.** `Exporter::runExport` (`editor/Exporter.cpp:534-617`) always takes
  these steps:
  - generate C++ with `copyGenerated` (`:1627`), `copyCppScripts` and `copyEngine` (`:2125`);
  - `configureBuild` (`:791`) and `runBuild` (`:928`), with the user's CMake/compiler or emsdk;
  - `collectDesktopArtifacts` and `collectWebArtifacts` (`:987`, `:1336`).

  `ExportMode` is only `{SourceCode, Desktop, Web}` (`editor/Exporter.h:30`). The Build Settings
  window can build a player only for the host desktop and Web
  (`ExportWindow::platformCanBuildPlayer`, `editor/window/dialog/ExportWindow.cpp:407`). Android,
  iOS and the other desktops get a source project (Android Studio or Xcode workspace,
  `engine/workspaces/{androidstudio,xcode}`).
- **Scenes are compiled C++.**
  - `Factory::createScene` writes `.jasperengine/generated/*.cpp` (`editor/Generator.cpp:1999-2010`).
  - `main.cpp` registers scenes and prefabs through
    `SceneManager::registerScene(id, name, factory, …)` (`engine/core/manager/SceneManager.h:124-128`)
    and `PrefabManager::registerPrefab`.
  - `copyGenerated` patches the start scene by string replacement (`Exporter.cpp:1684-1705`).
  - The runtime has no YAML and no data-driven scene loader. The editor's YAML codec
    (`editor/Stream.cpp`, 46 `decode*Component` functions) depends on `Project`/`SceneProject`.
- **Project settings are compile-time macros.**
  - Window settings: `JASPERENGINE_WINDOW_MODE/RESIZABLE/TITLE/APP_ID`, `VSYNC`
    (`engine/platform/linux/JasperEngineLinux.cpp:26-44`).
  - HybridArray capacities: `MAX_SUBMESHES`, `MAX_BONES`, … (`buildSceneMaxValuesDefinitions`,
    `Exporter.cpp:2502`, defaults in `engine/core/Engine.h:80-142`).
  - The backend: `GRAPHIC_BACKEND` (`engine/CMakeLists.txt:104-116`).
  - Canvas, scaling, loading and layer settings are baked into the generated `init()`.
- **Content is already data**, which the player builds on:
  - Assets, Lua and shaders resolve through `asset://`, `lua://` and `shader://`
    (`engine/core/io/FileData.cpp:119-142`). Desktop defaults are paths relative to the *working
    directory* (`SystemLinux.cpp:112-137`), not to the executable.
  - Shaders can load from `.sdat` files (`ShaderPool::loadPrecompiledShader`,
    `engine/core/pool/ShaderPool.cpp:786-797`; the export writes them with
    `ShaderOutputFormat::Binary`).
  - An experimental packed format exists: `resources.pak`, magic `DXPK1`, obfuscated, file-backed
    only (`engine/core/io/ResourcePack.{h,cpp}`).
- **Web** links assets with `--preload-file` into one `.data` file, built per project.
  `EMSCRIPTEN_THREAD_SUPPORT` defaults OFF (`engine/CMakeLists.txt:208-212`). There is no
  COOP/COEP solution.
- **Android** builds with Gradle plus NDK/CMake.
  - Template: `engine/workspaces/androidstudio/app/build.gradle`. It has 4 ABIs, R8, namespace
    `com.jaspergameengine.jasperengine`, and GameActivity dependencies (the built-in ads SDK was
    retired on 2026-11-04: ads go through the native bridge).
  - Settings: `AndroidProjectSettings` (`editor/Project.h:539`), which `writeAndroidProjectSettings`
    patches into Gradle text.
- **Runs.**
  - `tools/smoke-test.sh` exports a source project, *compiles* it with CMake, waits for
    `Engine ready:` (`Engine.cpp:1147-1212`, `JASPERENGINE_READY_SIGNAL`), then screenshots the
    whole X screen with `import`.
  - The MCP `run_build` (`editor/mcp/tools/BuildTools.cpp:1903`) screenshots on Linux/X11 only.
  - The engine already has `Engine::setFixedDeltatime` (`Engine.cpp:655`) and `System::quit()`
    (`System.h:124`).
- **Notices.** Exports ship only `licenses/shaders.com-LICENSE.txt` (`writeEffectLicense`/
  `copyLicenses`, `Exporter.cpp:3424-3471`). The dav1d, opus, libwebm, lexbor, Jolt, Box2D, … notices
  are missing from shipped games.
- **Release.** CI uploads editor zips, AppImage and dmg (`.github/workflows/ci.yml:459+`). Games
  have no manifest, verification, symbol split or Steam tooling.
- **Reusable pieces.**
  - `editor/htmlui/HttpClient` (curl executable through `CommandRunner`).
  - `sha256Hex` (`engine/core/htmlui/compat/Hash.h:19`, whole buffer only).
  - nlohmann `engine/libs/json/json.hpp`, `engine/libs/stb/stb_image_write`.
  - The GL/Vulkan readback in `engine/tests/htmlui/harness/Readback.cpp`.

## 4. Reference (OpenEngine, MIT; read-only, never executed)

- **`Apps/Player/Source/PlayerRunOptions.{h,cpp}`** (123 lines).
  - Take: the flag names, strict whole-string number parsing (`TryParsePlayerDouble`/`Uint64`),
    refusing bad values with a message that says how to fix them, and `--silent-audio`.
  - Change: their `--headless` has no render device and refuses `--screenshot`. Ours renders to an
    unmapped window, so screenshots work (D9). We rewrite the code; their parser is tied to
    `GameConfig`.
- **`Tools/Scripts/publish_game.py`** (649 lines, stdlib Python). Port the logic to C++
  (`editor/publish/`), keeping attribution:
  - `tree_manifest` (case folding, symlink relocatability);
  - `binary_target` (ELF `e_machine`, PE machine, Mach-O cputype, fat magic);
  - `prepare` (temp-dir staging then rename, refuses overlapping storage, `steam_appid.txt` refusal,
    `.pdb`/`.debug` moved to `symbols/`);
  - `verify_release` (re-hash, "changed after preparation");
  - `vdf_string`/`steam_scripts` (distinct depots, no auto `SetLive` on `default`/`public`);
  - `deploy`/`REMOTE_INSTALL`/`REMOTE_ROLLBACK`/`REMOTE_SMOKE` (Deck over ssh with a kept
    previous build).

  Rewrite `validate_payload` for our layout (R30). Drop `editor_call`/`build_in_editor`, the C#,
  packages and `.shaderpkg` checks.
- **`Tools/Web/gepak.py`**, **`Apps/Player/Source/WebPackReader.cpp`** and
  **`Tools/Web/export_web_player.py`** (`player-manifest.json` schema).
  - Take: a project-agnostic wasm template, a fixed number of requests, the legal-path rule (no
    absolute paths, `..`, `\` or empty parts), and a pack hash in the manifest.
  - Improve: per-entry hashes, and reading in place instead of unpacking to MEMFS (D6).
- **`Apps/WebLibrary/coi-serviceworker.js`** v0.1.7 is third-party MIT (Guido Zuidhof). Its licence
  is verified in `Apps/WebLibrary/coi-serviceworker.LICENSE`, so we vendor it unmodified with that
  file (D7).
- **`cmake/StageThirdPartyNotices.cmake`**: take the pattern only (fail on a missing notice source,
  collect staged files). The code is vcpkg-share specific.
- **`docs/publishing/game-publishing.mdx`**: the user-facing flow (prebuilt template, recompile
  only for native code) is the model for our `docs/shipping.md`.

## 5. Requirements

### Functional — player runtime and cooked content

- **R1. Player target.** `cmake -S engine -DJASPERENGINE_PLAYER=ON` builds `jasperengine-player`
  from the existing platform layers (`engine/platform/*`) plus a new `engine/player/`.
  - At startup it reads `game.json` (D4) and registers every scene and prefab listed there with a
    factory that loads the cooked file.
  - It applies the window, canvas, scaling, vsync, loading, tags/layers, collision-matrix and audio
    (Project Settings > Audio and the project's mixer) settings from `game.json`, replacing the
    generated `init()`.
  - It then loads the start scene (or the one given by `--scene`).
  - In player builds every compile-time macro of §3 becomes a runtime value. The macro defaults
    stay for CMake builds.
- **R2. Content root.** Desktop players resolve `asset://`, `lua://` and `shader://` relative to the
  **executable's directory** (macOS: `Contents/Resources`), not the working directory.
  `--content DIR` overrides the root.
- **R3. Cooked scenes and prefabs.**
  - The editor writes `scenes/<id>.jscn` and `prefabs/<id>.jpfb` (format in D2) from the same
    in-memory entities that `Factory` sees after `loadAndSaveAllScenes()`, including baked prefabs
    (`Project::bakePrefabsForExport`).
  - Asset references are resolved paths (S04 §6.8). Editor-only components and `NoSave` fields are
    never written.
  - The runtime `CookedSceneLoader` creates entities and components and attaches Lua scripts with
    their property values.
  - Its result must be identical to the Factory path. AC4 proves this field by field and AC5 by
    screenshot.
- **R4. Runtime field table.** `engine/core/serialization/RuntimeFields.gen.inc` is generated from
  the S08 tables by `jasperengine-editor reflect --emit-runtime-fields <file>` and committed.
  - It holds `{fieldId, kind, setter}` per saved field.
  - Components with an S08 `ComponentCodec` (mesh/submeshes, tilemap, terrain, particles, UI
    document, …) get a hand-written runtime codec in `engine/core/serialization/codecs/`, paired
    with an editor encoder in `editor/cook/`.
  - CI regenerates the table and fails on any diff.
  - Unknown field ids are skipped with one warning per id (forward compatibility within a format
    major).
- **R5. Build-method eligibility.** `Exporter::chooseBuildMethod()` returns `Prebuilt` unless at
  least one of these holds, in which case it returns `Compile` with the reason shown in the UI, CLI
  and MCP:
  - (a) the project has C++ script sources (`isCppSourceFile` in a script dir);
  - (b) `ProjectBuild.cmake` exists;
  - (c) an aggregated `SceneMaxValues` exceeds the player's `capacities` in `player-info.json`;
  - (d) the project disables a compile-time feature that the player needs to know about at build
    time (`JASPERENGINE_PHYSICS_2D/3D` OFF is allowed: the player simply has them);
  - (e) the user forced Compile.

  When the user forces Prebuilt and the project is not eligible, the build fails with the list of
  reasons.
- **R6. Prebuilt build steps.**
  - Steps reused unchanged from the current pipeline:
    - `loadAndSaveAllScenes`, `resolveShaderKeys`, `copyLua`, `checkHtmlUIReferences`,
      `copyAssets` and `copyCustomShaderSources`;
    - `buildAndSaveShaders`, forced to `Binary` and only the selected backend;
    - `writeEffectLicense`.
  - New steps, in this order:
    1. `cookTextures` (S14 cache, target format set);
    2. `cookScenes`;
    3. `writeGameConfig`;
    4. `stagePlayer`;
    5. `packContent` (optional, D6);
    6. `stageNotices` (R27);
    7. `writeContentManifest`;
    8. the per-platform finalizer (R11–R15).
  - No CMake, compiler or emsdk is looked up on this path.
- **R7. Atomic output.**
  - The build is written to `<dest>.staging-<pid>` and then swapped in with S03
    `renameWithRetry`. The old build moves to `<dest>.previous` and is deleted after the swap.
  - A failed or cancelled build leaves the previous `<dest>` byte-identical.
- **R8. Player runtimes.**
  - A runtime is identified by `<platform>-<arch>-<backend>-<flavour>`, for example
    `linux-x64-glcore-release` or `web-wasm32-gles3mt-development`.
  - Flavours: `release`, plus `development` for S15 (profiler, HTML UI debugger, asserts).
  - Each runtime archive holds the player binaries, `licenses/`, a platform template (R12–R15) and
    `player-info.json` (D4).
  - The editor install ships the host's desktop runtimes and the web runtimes. Other runtimes are
    installed under `<user-data>/jasperengine/players/<editorVersion>/<id>/`.
- **R9. Download and install.**
  - "Download" fetches `jasperengine-player-<id>-<version>.zip` from the GitHub release of the
    editor's version through `HttpClient`.
  - It verifies the archive against the sha256 that is **compiled into the editor**
    (`PlayerRuntimeHashes.gen.h`, written by the release workflow; D5), extracts it to a temp dir,
    and renames it into place.
  - `players install <zip>` does the same offline.
  - A dev editor (no exact `v*` tag) has no hash table and accepts only local players. Those are
    found through `--players-dir` or `JASPER_PLAYERS_DIR`, built by `tools/build.sh players`.
- **R10. Version lock.**
  - A build uses only runtimes whose `engineVersion` equals the editor's
    `JASPERENGINE_EDITOR_VERSION` and whose `contentFormatVersion` major matches.
  - `--allow-player-mismatch` (CLI only, for engine developers) overrides this. It is recorded in
    `content-manifest.json`, and `publish verify` refuses it.

### Platform & backends

- **R11. Matrix.** CI builds these runtimes, in both flavours:

  | Target | Backends | Arch | Output |
  |---|---|---|---|
  | Windows | d3d11, glcore, vulkan | x64 | `<App>.exe`, static MSVC CRT (`/MT`), so no redistributable is needed |
  | Linux | glcore, vulkan, gles3 | x64 | ELF, RPATH `$ORIGIN`, glibc floor of `docker/linux.Dockerfile` (Ubuntu 22.04) |
  | macOS | metal, glcore | universal (arm64+x86_64) | `.app` template, ad-hoc signed in CI |
  | Android | gles3, vulkan | arm64-v8a, armeabi-v7a, x86, x86_64 | repack template (R13) |
  | iOS | metal | arm64 device, arm64+x86_64 simulator | `JasperEnginePlayer.xcframework` (static) and an Xcode template (R14) |
  | Web | gles3 (WebGL2), single-thread `gles3st` and pthreads `gles3mt` | wasm32 | template (R15) |

- **R12. Desktop finalizers.**
  - **Windows.**
    - The executable is renamed to `<App>.exe`.
    - On a Windows host the icon and `VERSIONINFO` are patched with `BeginUpdateResourceW`/
      `UpdateResourceW`.
    - On other hosts the exe keeps the engine icon and the build logs warning
      `W-ICON-NONWINDOWS`. The window and taskbar icon always come from content (`icon.png`)
      at runtime.
  - **Linux.** Writes `<App>` (exec bit set) and a `<App>.desktop` file.
  - **macOS.**
    - Content goes to `Contents/Resources`.
    - Writes the Info.plist keys (`CFBundleIdentifier`, name, versions, `LSMinimumSystemVersion`),
      and `AppIcon.icns` from the project icon with our own PNG-in-ICNS writer (`ic07`–`ic10`).
    - Then it re-signs:
      - with `codesign --force --options runtime --sign <identity|->` on a macOS host;
      - with `rcodesign sign` when that tool is on PATH (other hosts);
      - otherwise the build stays unsigned with blocking warning `W-MAC-UNSIGNED`, and
        `publish verify` refuses it (an unsigned arm64 binary will not launch).
- **R13. Android without NDK, CMake or Gradle.** The template archive holds:
  - `lib/<abi>/*.so`, linked with `-Wl,-z,max-page-size=16384` for 16 KB pages;
  - `classes*.dex`, from CI's Gradle and R8 build;
  - `res-flat.zip` (`aapt2 compile` output) and `stable-ids.txt` (`aapt2 --emit-ids`);
  - `AndroidManifest.template.xml`.

  The editor runs these steps:
  1. Writes the manifest from `AndroidProjectSettings` (label, orientation, permissions,
     backup, fullscreen, keep-screen-on, min/target SDK, version code and name).
  2. Compiles the launcher and adaptive icons with `aapt2 compile`.
  3. Links: `aapt2 link -I <sdk>/platforms/android-<targetSdk>/android.jar --manifest …
     --stable-ids stable-ids.txt --rename-manifest-package <applicationId> -A <staged assets> -o base.apk`.
  4. Adds the dex files and only the ABIs selected in settings. `.so` files are packed as the
     Android Gradle plugin packs them, so a prebuilt APK weighs what a compiled one does:
     - an APK whose `minSdk` is below 23 compresses them and says `extractNativeLibs="true"`, so
       they are extracted at install;
     - from `minSdk` 23, and always in a bundle, they are stored and loaded from the APK.
  5. Aligns with `zipalign -P 16 -f 4`.
  6. Signs with `apksigner sign` (v1+v2+v3 when `minSdk < 24`), then checks with
     `apksigner verify`.

  - **AAB:** `aapt2 link --proto-format`, then `java -jar bundletool.jar build-bundle`, then
    `jarsigner`.
  - **Package format** (Project Settings > Android > Package Format, `android.package_format`,
    `--android-format`, `build_project android_format`):
    - `aab` is the default. Google Play builds each device's APK from the bundle, carrying only
      that device's ABI.
    - `apk-per-abi` makes one APK per selected ABI (`<App>-<abi>.apk`), for distribution outside
      Play.
    - `apk` is the universal APK of every selected ABI, and only when chosen. Build Settings shows
      what it costs: the engine libraries of every ABI in one file, against at most the largest
      ABI's per device for the other two.
    - An APK is 16 KB aligned on its 64-bit ABIs (arm64-v8a, x86_64). armeabi-v7a and x86 keep
      4 KB pages, as Android 15 requires for 64-bit only.
  - **Requirements on the user's machine:** a JDK 17+ (`JAVA_HOME` or `java` on PATH), Android SDK
    `build-tools;35.0.0`+ (`aapt2`, `zipalign`, `apksigner`) and `platforms;android-<targetSdk>`.
    `bundletool` 1.17.x is downloaded once with a pinned sha256 (Apache-2.0, noticed).
  - **Keystore:** the keystore path and alias are project settings. Passwords are never stored:
    the editor prompts for them, or reads `JASPER_ANDROID_KS_PASS`/`JASPER_ANDROID_KEY_PASS`.
    Debug builds create `~/.android/debug.keystore` with `keytool` when it is missing.
  - Missing tools produce an actionable error naming the SDK Manager package. The build never falls
    back silently to Gradle.
- **R14. iOS needs Xcode** (stated in the UI). The template is an Xcode project that links the
  prebuilt xcframework, so no engine sources are compiled.
  - The editor stages content into `Content/` and writes Info.plist, bundle id, team id and icons.
  - On macOS with Xcode it runs `xcodebuild archive` and `-exportArchive` (method from settings:
    development, ad-hoc or app-store) and produces an `.ipa`. The simulator target builds an
    unsigned `.app`.
  - On other hosts, iOS produces the staged Xcode project only. The UI and CLI say "Open on a Mac
    with Xcode".
- **R15. Web template.**
  - Contents: `jasperengine-player-st.{js,wasm}`, `jasperengine-player-mt.{js,wasm}`,
    `index.html` (S02 shell, or the project's custom shell with `{{JASPERENGINE_*}}` markers),
    `player-manifest.json`, `content.jpak` and `coi-serviceworker.js`.
  - The loader picks `mt` only when `crossOriginIsolated`. Without it, it registers the service
    worker once (it reloads the page), and falls back to `st` when the worker cannot isolate.
  - The project setting "Multithreaded (requires cross-origin isolation)" (S06 R19) controls
    whether `mt` and the worker ship at all.
  - The wasm reads `content.jpak` in place from the heap (D6). IDBFS user data is unchanged.
- **R16. Backends.** Every backend in R11 passes AC5/AC6 on the CI jobs that run it today:
  - Linux glcore, gles3 and vulkan (lavapipe) on every push;
  - Windows d3d11/glcore/vulkan and macOS metal/glcore on demand;
  - web st and mt in the browser gate.

  No player feature needs compute shaders or SSBOs.

### Functional — headless run options (desktop players; web via URL query)

- **R17. Flags** (`engine/player/LaunchOptions.{h,cpp}`):
  - `--headless`: the window is never shown (X11 unmapped, Win32 not shown, macOS
    `NSApplicationActivationPolicyProhibited`). Audio uses the null backend and input devices are
    ignored. Rendering continues offscreen.
  - `--quit-after N`: exit after N drawn frames.
  - `--after-ready`: the frame counter starts at the frame that logs `Engine ready:`. Waiting gives
    up after `--ready-timeout S` (default 120 s).
  - `--fixed-fps F`: `setFixedDeltatime(1/F)`. Under `--headless`, vsync and frame sleeps are off.
  - `--screenshot PATH`: write a PNG of the final composited frame at the frame given by
    `--screenshot-frame K` (default: the last frame before quit).
  - `--resolution WxH`, `--scene <name|id>`, `--silent-audio`, `--content DIR`,
    `--log-level` (S09 R36), `--verify-content` (R21).
- **R18. Validation** follows OE's rules, with messages that say how to fix the problem:
  - a number must use the whole string;
  - `--quit-after` must be ≥ 1;
  - `--fixed-fps` must be > 0 and finite;
  - `--screenshot` needs a writable directory;
  - `--screenshot-frame` must be ≤ `--quit-after`;
  - an unknown `--` flag is an error (exit code 2). Arguments after `--` go to Lua
    (`System.args`).
- **R19. Exit codes.**

  | Code | Meaning |
  |---|---|
  | 0 | ok |
  | 2 | bad arguments |
  | 3 | error-level log seen with `--fail-on-error` |
  | 4 | screenshot capture or write failed |
  | 5 | content manifest or format mismatch |
  | 6 | not ready before the timeout |
  | 7 | no display (with "run under xvfb-run" on Linux) |

  Lua gets `Engine.isHeadless()` and `Engine.quit(code)`.
- **R20. Frame capture.** `engine/core/render/FrameReadback.{h,cpp}` replaces the harness's
  readback (which then uses it). It supports glcore, gles3, vulkan, d3d11 and metal. On the capture
  frame, the engine renders into `Engine` framebuffer mode and blits to the swapchain. PNGs are
  written with `stb_image_write`, rows top first.
- **R21. Content check at start.**
  - The player reads `content-manifest.json`. It exits with code 5 when:
    - the format major or `playerId` mismatches;
    - the start scene is missing;
    - any `.sdat` listed for its backend is missing.
  - Every entry read from a `.jpak` is checked against its FNV-1a 64.
  - `--verify-content` also checks the sha256 of every loose file before starting.

### Functional — publish and verify

- **R22. `publishing.json`** (project root, committed, schema v1) holds:
  - `targets{name: {platform, arch, backend, flavour, buildDir, executable, depotId?, deviceProfile?: "steamdeck", requiredFiles[]}}`;
  - `steam{appId, buildDescription}`;
  - `deck{host, user, remoteDir, keyFile?}`.

  The editor UI (Build Settings > Publish tab) edits it. Undo is not applicable because this is a
  file-level editor that saves explicitly.
- **R23. `publish prepare --release <name>`.**
  - Validates every target (R30).
  - Copies each one into `<releasesDir>/.preparing-<rand>/content/<target>/`.
  - Moves debug symbols into `symbols/<target>/`, using the method for the build:
    - `.pdb` files;
    - Linux: `objcopy --only-keep-debug` + `strip --strip-debug` + `--add-gnu-debuglink` when
      binutils exist;
    - macOS: `.dSYM` moved out *before* re-signing.
    - Prebuilt players ship pre-stripped, and their symbol archives
      (`jasperengine-player-<id>-<version>-symbols.zip`) are fetched into `symbols/<target>/player`
      when available.
    - Compile builds ship pre-stripped the same way: `JASPERENGINE_SPLIT_SYMBOLS`.
      - The link step splits the debug information into the build tree's `symbols/`: `<exe>.debug`
        with a debug link, `<exe>.dSYM`, or the PDB.
      - The build keeps a copy under `.jasperengine/symbols/<exe sha256>/`, keeping the five newest,
        and its `content-manifest.json` names that copy (`symbols`).
      - prepare copies it into `symbols/<target>/game`. A build without one gets
        `W-BUILD-SYMBOLS`.
      - Android's Gradle strips the libraries it packages itself. A release bundle carries their
        symbols for Play (`debugSymbolLevel 'FULL'`).
  - Re-validates, writes `release.json` (D11), and renames the staging dir to `<releasesDir>/<name>`.
  - Refuses an existing release name, and release storage that overlaps a build dir.
- **R24. `publish verify <release>`.** Re-runs R30 on `content/<target>` and compares
  `tree_manifest` with `release.json`. Any difference fails with
  `Release content changed after preparation: <target>/<path>`.
- **R25. Steam.**
  - `publish steam-vdf [--preview] [--branch b]` writes `steampipe/app_build_<appId>.vdf` and
    `depot_build_<depot>.vdf` with `vdf_string` escaping.
  - It refuses control characters, duplicate depot ids, and `SetLive` on `default`/`public`.
  - `publish steam-upload` runs `steamcmd +login <user> +run_app_build <vdf> +quit`, after
    `verify`. Credentials stay in SteamCMD's own cached session; the editor never stores them.
- **R26. Steam Deck.**
  - `publish deck-deploy <release> --target <t>` copies the content over `ssh`/`scp` (system
    OpenSSH) to `<remoteDir>/<release>.incoming`, swaps it in, and keeps exactly one previous
    build.
  - `deck-rollback` restores the previous build.
  - `deck-smoke` runs `<exe> --quit-after 600 --fail-on-error` remotely and returns the exit code
    and log tail.
  - Deck targets must be `linux-x64`. Verify reports the highest `GLIBC_x.y` version needed by the
    player and its `.so` files (ELF `.gnu.version_r`).
- **R27. Notices.**
  - Every build contains `licenses/` with one `<lib>-LICENSE` per library linked into that player,
    plus `THIRD-PARTY-NOTICES.txt` generated from the `NOTICE.md` rows.
  - `engine/notices.json` maps `lib → licence file, condition` (always, physics2d, physics3d,
    vulkan, htmlui, video, ads, …). CI writes each player's list into `player-info.json`
    `notices[]`.
  - The Compile path computes the list from the CMake options.
  - S02's `check-notices.py` fails CI when an `engine/libs/*` directory has no `notices.json`
    entry.
  - Vendored `coi-serviceworker.LICENSE` and `bundletool` are listed.

### Performance budgets

- **R28.**
  - **Prebuilt build time.** Building `tools/fixtures/unified-lighting` for Linux, with a warm
    S14/shader cache, takes ≤ 20 s on a 4-vCPU runner. The Compile path takes minutes.
  - **Scene load.** Cooked scene decode is ≤ 1.5× the Factory C++ load time, or ≤ 100 ms
    (whichever is larger) for a 10,000-entity synthetic scene on desktop, and ≤ 400 ms on the
    Android reference device.
  - **Web.** Per-entry FNV checks cost ≤ 1 ms/MB in Chrome. Time to `Engine ready:` is no worse
    than +5 % versus the `--preload-file` build of the same fixture.
  - **Headless.** Under `--headless --fixed-fps 60`, 600 frames of the engine sample render in
    ≤ 15 s wall time on llvmpipe, with no sleeps.
  - **Publish.** `publish verify` hashes at ≥ 200 MB/s on one core (streaming SHA-256 added to
    `engine/core/htmlui/compat/Hash`, moved to `engine/core/util/Sha256.{h,cpp}`).
  - **Zero runtime cost.** A player started without the R17 flags runs the same per-frame code as
    today's build.

### Editor UX (human)

- **R29. Build Settings** (`ExportWindow`).
  - All six platforms become buildable.
  - Each platform row shows its runtime state: Installed, or "Download (NN MB)" with progress and
    cancel.
  - A **Build Method** row shows "Prebuilt player (no compiler needed)" or
    "Compile from source: <reason>", with an override combo (Auto / Prebuilt / Compile).
  - A **Development Build** checkbox (S15) selects the flavour.
  - Android adds an APK/AAB choice, Keystore… and a tool check list (JDK, build-tools, platform:
    each ✓ or a "Set path" link to Preferences > External Tools). iOS shows "Requires a Mac with
    Xcode".
  - **Build** and **Build And Run** keep their names.
  - A new **Publish** tab holds the targets table, Prepare, Verify, Steam VDF, Upload…, Deploy to
    Deck and Rollback. Each runs as a cancellable job with the log in the Console.
  - Preferences > External Tools adds Player Runtimes (list, size, Remove, Install from file…),
    Android SDK, JDK, steamcmd and rcodesign paths.
  - Project Settings edits are undoable through the existing settings commands.

### Agent UX (MCP, CLI, Lua)

- **R30. Payload validator** (`editor/publish/PayloadValidator.cpp`; used by build, prepare and
  verify). It fails, with every finding listed, on:
  - an executable or library whose architecture or format doesn't match the target. ELF, PE and
    Mach-O headers are parsed; fat binaries must hold every slice; Android `lib/<abi>/*.so` must
    match its ABI directory; wasm needs the `\0asm` magic;
  - a missing exec bit (Linux, macOS);
  - case-colliding paths;
  - absolute symlinks, or symlinks that escape the tree. Windows and web targets allow no
    symlinks at all;
  - Windows-illegal names (`CON`, `NUL`, …, trailing dot or space, `:`), or paths of 240 characters
    or more, on Windows targets;
  - `steam_appid.txt`;
  - a missing `game.json`, `content-manifest.json`, start scene, scene, prefab or `.sdat` listed in
    the manifest;
  - a missing `licenses/` entry from `notices[]`;
  - `allowPlayerMismatch`;
  - an unsigned macOS arm64 slice (no `LC_CODE_SIGNATURE`);
  - for Android: `apksigner verify` failure, or a 64-bit ABI's `.so` that is not 16 KB aligned.
- **R31. CLI** (`editor/cli/CommandLine.cpp`). `export` (source project) is unchanged. New
  commands:
  - `build --project P --platform <windows|linux|macos|android|ios|web> --out D [--backend b] [--method auto|prebuilt|compile] [--development] [--pack] [--android-format aab|apk-per-abi|apk] [--start-scene s] [--json]`
  - `players list|install <zip>|download <id>|remove <id> [--players-dir D]`
  - `publish init|prepare|verify|steam-vdf|steam-upload|deck-deploy|deck-rollback|deck-smoke …`
  - `reflect --emit-runtime-fields <file>`

  All of them are headless (no GUI), print stable `key: value` lines or `--json`, and exit non-zero
  on failure.
- **R32. MCP** (`editor/mcp/tools/BuildTools.cpp`, plus a new `PublishTools.cpp`).
  - `build_project` changes:
    - `mode` gains `player`, which covers all platforms. `desktop` and `web` stay as aliases.
    - New params: `platform`, `method`, `development` and `android_format`.
    - The result adds `method_used`, `compile_reasons[]`, `player_id`, `content_manifest` and
      `warnings[{code,message}]`.
  - `get_build_info` adds `players[{id, installed, size_mb}]` and per-platform `tools_missing[]`.
  - New tools:
    - `install_player_runtime {id}`, as a job;
    - `publish_prepare {release, targets?}`, `publish_verify {release}` (readOnly) and
      `publish_steam_vdf {release, preview, branch?}`;
    - `publish_deploy_deck {release, target}` (openWorld). `publish_steam_upload` is
      deliberately **not** exposed (a human runs it).
  - `run_build` gains `headless`, `quit_after`, `fixed_fps`, `after_ready` and
    `screenshot_frame`. It returns the player's own `--screenshot`, so it works on Windows and
    macOS and needs no X11 `import`.
  - The guide (`docs/mcp/guide/`) gets a "Shipping" page.
- **R33. Smoke test.** `tools/smoke-test.sh` gains `SMOKE_METHOD=prebuilt|compile` (default
  `prebuilt` once AC1 passes). It runs the player with
  `--headless --after-ready --quit-after 30 --fixed-fps 60 --screenshot game.png --fail-on-error`
  instead of timed `import` screenshots. `SMOKE_GAME_RUNS` maps to `--scene`/env as today.

### Data & compatibility

- **R34.** New files carry `schemaVersion`/`formatVersion`. Readers refuse a newer major with a
  message that names the editor version needed. The files are:
  - `game.json`, `content-manifest.json`, `player-info.json`;
  - `.jscn`/`.jpfb`, `.jpak`;
  - `publishing.json`, `release.json`.
- **R35.** New `project.yaml` keys default to today's behaviour, and old projects load unchanged:
  - `build.method: auto`, `build.development: false`, `build.pack: false`;
  - `android.packageFormat: apk`, `android.keystore{path,alias}`;
  - `ios.teamId`, `ios.exportMethod`;
  - `web.multithreaded` (S06).

  Nothing in `*.scene` changes, so no scene version bump is needed.
- **R36.** `resources.pak` (`DXPK1`) stays readable. New native packs are written as `.jpak` v1,
  and `packNativeResources` writes `content.jpak`.

## 6. Design constraints & decisions

- **D1. Two methods, one pipeline.** `ExportConfig` gains `BuildMethod method {Auto, Prebuilt,
  Compile}`, `BuildPlatform platform`, `bool development` and `AndroidFormat`.
  - `runExport` shares every content step. Prebuilt replaces `copyGenerated` through `runBuild`
    with R6 steps 1–8. Compile also writes `content-manifest.json` and notices, so `publish`
    treats both outputs the same.
  - The Generator/Factory C++ path is kept: Play and Compile still use it.
- **D2. `.jscn` v1.** Little-endian. Layout:
  - header `JSCN`, format version, sceneId, entity count, string-table offset;
  - entity records `{entityId, parentIndex, nameStr, componentCount}`;
  - component records `{componentKeyHash32, byteLength, fields…}`;
  - field records `{fieldId = fnv1a32("<key>.<yamlKey>"), kind u8, payload}`. The kinds follow S08
    `Kind`. Strings and paths are string-table indices; arrays are a count followed by the
    elements;
  - script records (`lua path`, property records);
  - scene settings (S03/S12 scene-level values).

  A record's byte length lets the loader skip it. This is a binary encoding of S08's tables, not a
  second schema. YAML/yaml-cpp never ships in players.
- **D3. Player code layout.**
  - `engine/player/PlayerMain.cpp`: shared by every platform's `main`, under
    `JASPERENGINE_PLAYER`.
  - `PlayerConfig` (`game.json`), `LaunchOptions`, `CookedSceneLoader`, `ContentManifest`.
  - The platform `main.cpp` files call `PlayerMain::init(argc, argv)` when the option is ON.
    Otherwise they keep today's generated `init()`.
- **D4. File shapes** (schema v1, abridged).

  ```jsonc
  // game.json
  {"schemaVersion":1,"name":"My Game","appId":"com.me.game","startScene":3,
   "scenes":[{"id":3,"name":"Main","file":"scenes/3.jscn","involved":[3,5],"active":[3]}],
   "prefabs":[{"id":1,"guid":"…","file":"prefabs/1.jpfb"}],
   "window":{"mode":"windowed","width":1280,"height":720,"resizable":true,"title":"My Game","vsync":true},
   "canvas":{"width":1280,"height":720,"scaling":"fitwidth","textureStrategy":"none"},
   "loading":{"scene":0,"delay":0,"timeout":5,"async":false},"htmlUiMsaa":4,
   "tags":[],"layers":[],"collision3D":[…],"collision2D":[…],
   "audio":{"mixer":"audio/Main.mixer","maxRealVoices":32,"maxVoices":256,"sampleRate":0}}
  // content-manifest.json
  {"schemaVersion":1,"engineVersion":"v1.4.0","contentFormatVersion":"1.0","playerId":"linux-x64-glcore-release",
   "playerSha256":"…","method":"prebuilt","backend":"glcore","shaders":["MESH_…_glsl410.sdat"],
   "files":{"scenes/3.jscn":{"sha256":"…","size":1234}},"packs":[{"file":"content.jpak","sha256":"…","entries":812}],
   "allowPlayerMismatch":false}
  // player-info.json
  {"schemaVersion":1,"id":"linux-x64-glcore-release","engineVersion":"v1.4.0","gitSha":"…",
   "contentFormatVersion":"1.0","capacities":{"MAX_SUBMESHES":16,"MAX_BONES":128,…},
   "features":["physics2d","physics3d","htmlui","video"],"notices":["box2d","joltphysics",…],
   "executables":{"jasperengine-player":"<sha256>"}}
  ```

  Capacities are the `Engine.h` defaults. A project that exceeds them compiles (R5c). Raising them
  is a later, measured decision.
- **D5. Player hashes.**
  - On `v*` tags the release workflow builds every runtime first (a `players` matrix that does not
    depend on the editor). It then writes `PlayerRuntimeHashes.gen.h` and builds the editor
    packages with it, so the editor trusts only archives of its own release.
  - On pushes, Linux and web runtimes are built and uploaded as artifacts, for AC1/AC2, without
    hashes.
  - The archive lists its own `player-info.json` sha256 values, which install re-checks.
- **D6. `.jpak` v1.**
  - Layout: header `JPAK\0\0\0\0`, u32 version, u32 count, u64 indexBytes; index records
    `{u32 pathLen, path, u64 offset, u64 size, u64 fnv1a64}`; then 16-byte-aligned blobs.
  - The legal-path rule from `gepak.py` applies, plus case-fold uniqueness.
  - The reader works on a file (native: `pread`) or on a memory span (web). It is the `Data::open`
    hook that `ResourcePack` provides today.
  - The writer lives in the editor, and `tools/jpak.py` is a stdlib inspector/unpacker for tests.
  - Obfuscation is dropped: it gave no security.
- **D7. COOP/COEP.** We vendor `coi-serviceworker.js` v0.1.7 unmodified under
  `engine/platform/emscripten/web/`, with its MIT LICENSE and a `NOTICE.md` row. This supersedes
  S06 R19's "write our own; licence unverified": the licence file is verified. S02's `serve.py`
  gains `--no-isolation-headers`, so the service-worker path can be tested.
- **D8. Platform tools are invoked, not reimplemented.** The editor shells out to `aapt2`,
  `zipalign`, `apksigner`, `bundletool`, `jarsigner`, `keytool`, `codesign`/`rcodesign`,
  `xcodebuild`, `objcopy`/`strip`, `ssh`/`scp` and `steamcmd`.
  - It always calls them through `CommandRunner` argument vectors (never a shell string).
  - Discovery covers the preference path, then the environment, then PATH. `get_build_info`
    reports each tool.
  - We implement only what is small and portable ourselves: the ICNS writer, the binary-header
    parsers, VDF and SHA-256. Zip reading and writing (player archives, APK/AAB assembly) uses
    vendored `miniz` 3.x (MIT, `libs/miniz`, editor only). The repo has no zip code today: the
    `miniz` mentions in `tiny_gltf.h` are optional hooks, not a vendored copy.
- **D9. Headless renders.** Unlike OE, `--headless` keeps the GPU device, because screenshot
  comparison is the point. On Linux it still needs an X server (Xvfb in CI), like the htmlui
  harness (`HeadlessContext`). A device-less mode (sokol dummy backend) is deferred.
- **D10. Publish in C++, not Python.** Users on Windows have no Python. One implementation backs
  the UI, the CLI and MCP, and CI calls `jasperengine-editor publish verify`. The logic is ported
  from `publish_game.py`, with the header "portions adapted from OpenEngine
  Tools/Scripts/publish_game.py (MIT)" and a `NOTICE.md` row.
- **D11. `release.json` v1.**
  `{schemaVersion, release, project, engineVersion, createdUtc, steam{appId}, targets{name:
  {platform, arch, backend, flavour, playerId, method, executable, depotId?, deviceProfile?,
  glibcFloor?, files{path:{sha256,size}|{symlink}}}}}`. Its content is reproducible apart from
  `createdUtc`.

API sketch:

```cpp
// engine/player/LaunchOptions.h
namespace jasperengine::player {
struct LaunchOptions { bool headless=false, afterReady=false, silentAudio=false, failOnError=false, verifyContent=false;
  uint64_t quitAfter=0, screenshotFrame=0; double fixedFps=0, readyTimeout=120;
  std::filesystem::path screenshot, content; int width=0, height=0; std::string scene, logLevel;
  std::vector<std::string> scriptArgs; };
struct ParseError { std::string message; };            // printed, exit 2
std::variant<LaunchOptions, ParseError> parse(std::span<const std::string> args);
}
// editor/publish/BinaryInfo.h
struct BinaryInfo { enum Format { Elf, Pe, MachO, Fat, Wasm, Unknown } format; std::vector<std::string> arches; bool signedMachO=false; std::string glibcFloor; };
std::optional<BinaryInfo> inspectBinary(const fs::path&, std::string* error);
// editor/publish/TreeManifest.h
struct TreeEntry { std::string sha256; uint64_t size; std::string symlink; };
std::expected<std::map<std::string, TreeEntry>, std::vector<std::string>> treeManifest(const fs::path& root, TargetOs);
```

## 7. Acceptance criteria

- **AC1. Linux, no compiler.** CI job `export-prebuilt-linux` runs in the new
  `docker/runtime.Dockerfile` (Ubuntu 22.04, Mesa llvmpipe/lavapipe, Xvfb, Python; **no** gcc,
  clang or cmake).
  - It first asserts `! command -v cc c++ cmake`.
  - Then it runs `jasperengine-editor build --platform linux --backend opengl --project tools/fixtures/unified-lighting --out out`,
    with players from the `players` artifact.
  - Then it runs
    `xvfb-run out/<App> --headless --after-ready --quit-after 30 --fixed-fps 60 --screenshot game.png --fail-on-error`.
  - Pass condition: exit 0, and `pngdiff.py game.png tools/fixtures/unified-lighting/golden/prebuilt-linux-glcore.png --tolerance 24 --max-fraction 0.01`
    exits 0.
  - The same job runs for `gles3` and (nightly) `vulkan`.
- **AC2. Web, no emsdk.** In the same image, `build --platform web` runs with `EMSDK` unset.
  - `browser_gate.py --root out --page index.html --until-ready --baseline tools/web/gates/prebuilt-unified-lighting.json --screenshot web.png`
    passes (S02 R17).
  - It passes once with `serve.py` headers (`isolated: true`, `mt` loaded) and once with
    `--no-isolation-headers` (the service worker isolates after one reload, `mt` is loaded).
  - With `web.multithreaded: false`, the build has no `-mt` files and `isolated: false` passes.
- **AC3. Fallback.**
  - `build --platform linux` on `tools/fixtures/prefab-runtime` (which has C++ scripts) prints
    `method: compile` and `compile_reason: C++ scripts (…cpp)`. In the no-compiler image it fails
    with exit 1 and the message `needs a C++ toolchain`.
  - A synthetic project with `MAX_BONES` 200 reports reason (c).
  - `--method prebuilt` on either one fails and lists the reasons. *(MCP test
    `tools/mcp/tests/test_build_method.py`, CI.)*
- **AC4. Cook round trip.** `CookedScene.RoundTripEveryComponent` (`jasperengine-editor-tests`)
  checks every S08 `ComponentDesc` with non-default randomized values (seeded):
  - it cooks the values, decodes them with the runtime loader, and compares them with S08 epsilon
    equality;
  - it covers every codec component with its fixture.

  `RuntimeFields.Generated` fails when the committed `.gen.inc` differs from a fresh emit.
- **AC5. Parity.** In the nightly `e2e` job, `smoke-test.sh` runs every Lua-only
  `tools/fixtures/*` project with `SMOKE_METHOD=compile` and with `SMOKE_METHOD=prebuilt`. Each
  pair of screenshots matches with `pngdiff.py --tolerance 8 --max-fraction 0.002`, for glcore and
  gles3.
- **AC6. Headless options.** The `LaunchOptions.*` gtests cover every R18 refusal (exit 2 text),
  the `--` passthrough and the defaults.
  - A process test (`tools/ci/test_player_cli.py`) runs the engine sample player and checks:
    - `--quit-after 120 --fixed-fps 60` exits 0 after exactly 120 frames (logged count);
    - `gameTime` is 2.000 s ± 1e-9;
    - `--fail-on-error` on a fixture with a Lua error exits 3;
    - no `DISPLAY` exits 7;
    - `--screenshot` writes a 1280×720 PNG whose centre pixel matches the fixture baseline.
  - The same `quit-after`/screenshot check runs on Windows (d3d11) and macOS (metal) in the
    on-demand jobs.
- **AC7. Android, no NDK.** The `prebuilt-android` CI job runs `tools/ci/android_ac7.sh`. It
  points `ANDROID_NDK_HOME` nowhere and puts stand-ins first on PATH for `cmake` and `ndk-build`;
  they fail and log if anything runs them. It then:
  - builds `tools/fixtures/unified-lighting` with `--platform android` once for each
    `--android-format`: `aab`, `apk-per-abi` and `apk`;
  - checks that no stand-in ran;
  - `apksigner verify --verbose` passes (v2 signed);
  - `zipalign -c -P 16 4` passes;
  - `aapt2 dump badging` shows the project's applicationId, versionCode and label;
  - `bundletool validate` passes on the `.aab`;
  - `unzip -l` shows only the ABIs selected in the fixture settings.

  Running on an emulator is manual and covered by the release checklist.
- **AC8. iOS.** The macOS on-demand job builds the fixture for iOS simulator from the prebuilt
  xcframework. `xcodebuild` compiles no engine `.cpp` (the log has none), and the `.app` boots in
  the simulator with `xcrun simctl launch` (log line `Engine ready:`). On Linux, `build --platform ios`
  produces the Xcode project and prints the "needs a Mac with Xcode" note.
- **AC9. Validator negatives.** `PayloadValidator.*` gtests use synthetic trees and headers. Each
  case below fails with its named message, and a clean tree passes:
  - an ELF aarch64 binary in an x64 target;
  - a PE i386 binary;
  - a fat binary missing arm64;
  - `Foo.png` next to `foo.png`;
  - a `../../etc` symlink, an absolute symlink, and any symlink in a Windows target;
  - `NUL.txt`;
  - `steam_appid.txt`;
  - a removed `.sdat`;
  - a missing start scene;
  - a missing `licenses/dav1d-LICENSE`;
  - an unsigned arm64 Mach-O.
- **AC10. Prepare/verify.** `tools/mcp/tests/test_publish.py` (CI) runs `publish_prepare` on the
  AC1 output and checks that the symbols are in `symbols/` and absent from `content/`. Then:
  - `publish_verify` passes;
  - after one byte of `content/linux/assets/x.png` changes, verify fails with
    `changed after preparation: linux/assets/x.png`;
  - a second prepare with the same name is refused;
  - storage inside the build dir is refused.
- **AC11. Steam/Deck.**
  - `PublishSteam.*` gtests compare the VDF output with golden text, including the escaping of
    `"` and `\`. They also check that control characters, duplicate depots and
    `--branch public` (with `SetLive`) are refused.
  - The Deck deploy/rollback is tested against a local `sshd` container in the nightly job:
    deploy, deploy again, rollback, then check that the remote dir holds the first build's
    manifest.
- **AC12. Notices.** Every AC1/AC2/AC7 output contains `licenses/` matching `player-info.json`
  `notices[]` and `THIRD-PARTY-NOTICES.txt`. `check-notices.py` fails when an `engine/libs/` dir
  is missing from `notices.json`. *(`tools/ci/test_check_notices.py` extended.)*
- **AC13. Atomic output.** An MCP test cancels a build after `cookScenes`. The previous output dir
  hashes identically before and after, and no `.staging-*` dir remains after the next successful
  build.
- **AC14. Budgets.** CI logs these values and the job fails over budget:
  - the AC1 build time (≤ 20 s warm);
  - `tools/performance/cooked_scene_load.cpp` (≤ 1.5× Factory, or ≤ 100 ms);
  - the 600-frame headless run (≤ 15 s);
  - `publish verify` throughput (≥ 200 MB/s on the 1 GB synthetic tree).
- **AC15. Downloads.**
  - `PlayerInstall.*` gtests: the hash-mismatch archive is refused and leaves no partial dir; a
    good archive installs atomically; an archive with a `..` entry is refused.
  - An MCP test installs a runtime from a local file with `install_player_runtime`.
- **AC16. Docs.** `docs/shipping.md` (user flow, tool requirements per platform, signing, Steam,
  Deck) and the MCP guide "Shipping" page exist. `gen_tool_docs.py` lists the new tools.

## 8. Dependencies & conflicts

- **Depends on:**
  - S01 (C++20: `std::expected` where available, else our `Result`);
  - S02:
    - Docker images, `tools/build.sh` (new `players` and `runtime-image` targets);
    - `browser_gate.py` and `serve.py` (S30 adds `--no-isolation-headers`);
    - `check-notices.py`, the gtest gate, nightly;
  - S03 (`publishFile`/`renameWithRetry` for staging and installs);
  - S04 (resolved paths and `assetmap.bin` in content);
  - S06 (R19 pthread pool sizes; D7 supersedes its service-worker note);
  - S08 (`ComponentDesc` tables and codecs; the runtime field emitter is S30's);
  - S09 (`--log-level`);
  - S14 (per-target texture cook used by `cookTextures`);
  - S15 (the development flavour);
  - S28 (crash handler linked into players).
- **Blocks / consumed by:**
  - S19 AC27 (a headless player runs `nav_tilemap`);
  - S27 movie recorder (reuses `FrameReadback` and `--fixed-fps`);
  - S31 (`call` CLI exposes the publish tools).
- **Merge hotspots:**
  - `editor/Exporter.{h,cpp}` (S01, S02, S06, S14, S15);
  - `editor/window/dialog/ExportWindow.cpp`;
  - `editor/mcp/tools/BuildTools.cpp` (S31);
  - `editor/cli/CommandLine.cpp` (S31);
  - `engine/CMakeLists.txt` (S01, S02, S06, S14, S15);
  - `engine/platform/*/main.cpp` and `JasperEngine*.cpp` (S28, S09);
  - `engine/core/Engine.cpp` (S03, S06, S09, S28);
  - `engine/core/io/ResourcePack.*`;
  - `.github/workflows/ci.yml` (S02);
  - `tools/smoke-test.sh` (S02);
  - `NOTICE.md`.

## 9. Risks & defaults

- **Biggest risk: cooked-scene parity with the Factory path.** Two producers can drift, so a
  prebuilt game could differ from Play and Compile.
  - Mitigations: one S08 table drives both outputs; the round trip (AC4) and the pixel parity
    (AC5) tests are mandatory gates; codecs are paired editor/engine files reviewed together.
  - Default: if AC5 fails for a component, `chooseBuildMethod` lists it as a Compile reason until
    it is fixed. No silent divergence ships.
- **Player download size and release time.** About 30 runtimes × 2 flavours. Defaults:
  - per-runtime archives, so only the host and web ship in the installer;
  - release builds take longer (players are built before editors);
  - PRs build only Linux and web runtimes.
- **Android tool churn** (aapt2 flags, 16 KB pages, `bundletool`). Defaults: pin build-tools ≥
  35.0.0 and bundletool 1.17.x. AC7 runs on every push, so drift shows up quickly.
- **macOS signing from non-Mac hosts.** Default: rcodesign if present; otherwise the build is
  unsigned with a warning and publish refuses it. Notarization stays manual (`xcrun notarytool`,
  documented).
- **Windows exe icon from non-Windows hosts.** Default: the warning `W-ICON-NONWINDOWS`. A portable
  `.rsrc` rewriter is a follow-up if users ask.
- **Fixed capacities in players.** Default: the engine defaults, with a Compile fallback (R5c).
  Revisit with memory measurements in S07/S15.
- **Determinism of headless screenshots** (async loads, RNG, the time-based shader compile).
  Defaults: `--after-ready` plus a fixed dt plus tolerant `pngdiff`. `math.randomseed` is not
  forced; fixtures that need it seed themselves.
- **Zip writer.** Default: vendor `miniz` 3.x (MIT) in `libs/miniz` for the editor only, with a
  `NOTICE.md` row. Players never link it.

## Re-check (2026-10-29, oe-port at 0c22d3b91)

The spec was written before three rounds of upstream catch-up and the landing of S02-S09,
S14/S14b, S15, S19-S28 and S31. What the tree has now, and what S30 does about it:

- **Exporter** (`editor/Exporter.{h,cpp}`, now 4,581 lines; §3 line numbers are stale).
  `runExport` runs `prepareCacheTarget`/`checkTargetDir`, `clearGenerated`,
  `loadAndSaveAllScenes`, **`checkMissingAssets` (S04)**, `resolveShaderKeys`, `copyGenerated`,
  `copyLua`, `checkHtmlUIReferences`, **`cookTextures` (S14, `assets/je-cooked/<family>`)**,
  `copyAssets`, **`writeAssetMap` (S04, `assetmap.bin`)**, `copyCustomShaderSources`,
  `copyCppScripts`, `copyEngine`, `buildAndSaveShaders`, `writeEffectLicense` (now three notices:
  shaders.com, WebGL fluid, post-FX) and, for builds, configure/build/collect. S30's Prebuilt
  method reuses all content steps up to `writeEffectLicense` unchanged (R6) and replaces only
  `copyGenerated`, `copyCppScripts`, `copyEngine` and configure/build/collect.
- **Web threads and COOP/COEP (S06, landed).** `ExportConfig::webMultithreaded` (Project
  Settings > Player > Threading > Web: Multithreaded) builds a second Emscripten tree
  (`build-threads`) into `threads/`, writes `_headers` and S06's **own** cross-origin isolation
  service worker (`COI_SERVICE_WORKER`, inline in `Exporter.cpp`) and swaps the page's
  `<script src>` for a loader. The loader starts the single-thread player *and* reloads at the
  same time when not isolated (a reload in the middle of a load), and nothing tests a server
  without isolation (S06 AC21's open half). **Decision (supersedes D7):** keep S06's first-party
  worker (no third-party notice needed, nothing vendored) but move it to
  `engine/platform/emscripten/web/coi-serviceworker.js` and the loader to
  `engine/platform/emscripten/web/player-loader.js`, shared by the Compile and Prebuilt web
  outputs; the loader decides once (isolated: `mt`; not isolated and a worker can be installed
  and was not tried this session: register, wait for it to control, reload, without starting a
  player; otherwise `st`). The browser gate covers threaded, worker-isolated and
  worker-blocked single-thread runs; this closes S06 AC20/AC21.
- **S02** landed `tools/web/serve.py` and `browser_gate.py` with **`--no-isolation`** (the spec's
  `--no-isolation-headers`: we use the existing name), baselines with `isolated`, `query`,
  `init_script`, `expect`, `console`, `allow`; `tools/mcp/pngdiff.py` (the spec's
  `pngdiff.py`); `tools/ci/check-notices.py` (NOTICE.md rows + licence files, `--installed`,
  `--install-manifest`, says "S30 reuses this for exported games"); `tools/ci/gtest-gate.py`;
  the nightly workflow (`linux`, `web-threads`, `report`); docker images `linux`,
  `linux-backends`, `web`, `android`, `android-emulator` (no runtime image yet: S30 adds
  `docker/runtime.Dockerfile`). `engine/notices.json` builds on check-notices' library list
  instead of a second scanner.
- **S03** `engine/core/io/AtomicFile.h`: `writeFileAtomic`, `publishFile`, `renameWithRetry`,
  `makeTemporarySiblingPath`; used for staging swaps, installs and release renames.
- **S04** GUIDs, `assetmap.bin`, missing-asset check: cooked scenes carry resolved paths, the
  asset map ships in content as today.
- **S08** reflection (`editor/reflect/`): `Kind`, `FieldDesc` (member thunks, `cppPath`, flags
  `NoSave`/`Proxy`/`Transient`/`Derived`/`OmitIfDefault`…), `StructDesc`, `ComponentDesc` with
  optional `ComponentCodec` and `codegen` (35 components keep a codec or a Factory codegen:
  mesh, model, terrain, tilemap, sprite, UI, script, particles, skybox, physics bodies,
  navigation, animation tracks, lattice, pose, gameplay…). The runtime field emitter (R4) reads
  these tables; components whose table holds every saved key (the S22 gameplay ones, force
  field, navigation agents…) need no hand-written runtime codec, only the generic table path.
- **S09** `--log-level` is already parsed by `Engine::systemInit` (`startGameLogger`); the
  player's LaunchOptions accept and forward it rather than parse it twice.
- **S15** Development Build (`ExportConfig::developmentBuild`, `Project::isDevelopmentBuild`,
  `JASPERENGINE_DEVELOPMENT_BUILD`, `--profile*` flags): the `development` flavour is that.
- **S28** crash reporting is installed by `Engine::systemInit` for every non-editor build
  (`startGameDiagnostics`); its product name comes from the `JASPERENGINE_WINDOW_TITLE`
  macro. Players link it unchanged and set the product name from `game.json` at runtime.
- **S19** navigation, **S20** particles, **S21/S22**, upstream lattice/fluid/pose/water/FBX: new
  components the cook covers through their tables and codecs.
- **S31** MCP: tool groups (`McpTools.cpp` module rows), `tools/mcp/budget.json`; the core listing
  is ~98.3k of 100k characters and the guide 16,366 of 16,384 bytes. New tools go in a
  `shipping` group; `build_project`/`run_build`/`get_build_info` (core) grow by as few schema
  characters as possible, paid for by shortening their own descriptions; the guide gets one
  line and `docs/mcp/guide/shipping.md` the detail.
- **Still true:** every build compiles; `ExportMode` is `{SourceCode, Desktop, Web}`; Build
  Settings builds players only for the host desktop and web; scenes are generated C++; project
  settings are compile-time macros (window, vsync, capacities in `Engine.h`, now also
  `MAX_MESH_LODS`, `MAX_TEXT_FONTS`, `MAX_MORPHTARGETS`); `resources.pak` is `DXPK1`; desktop
  content paths are relative to the working directory; `run_build` screenshots with X11
  `import`; the smoke test compiles (and now also opens the editor and runs S19's
  `nav_smoke.py`); no zip code in the repository (basisu's `basisu_miniz.h` has no archive API),
  so miniz 3.0.2 is vendored as D8 says.
- **Fixtures:** `tools/fixtures/unified-lighting` (Lua only) and `tools/fixtures/prefab-runtime`
  (C++ scripts in `cpp/`) exist as AC1/AC3 expect.

## Progress

Kept current so the work can continue after an interruption.

- **Done, built, tested (core):** streaming SHA-256 (`engine/core/util/Sha256`), FNV
  (`util/Fnv.h`), `.jpak` reader (`io/JPak`) mounted through `ResourcePack`, `content://` and
  `System::getContentPath`, `PlatformConfig` filled by `platform/common/PlatformDefaults.h`,
  `player::launch` / `LaunchOptions` / `RunControl` (exit codes 0/2/3/4/5/6/7), `FrameReadback`,
  headless on Linux/Windows/macOS, Lua `Engine.isHeadless()`, `Engine.quit(code)`,
  `System.args`. Core tests `Sha256.*`, `JPak.*`, `LaunchOptions.*`, `RunControlFixture.*` pass
  (B); `ReadyTimeoutExitsSix` was timing-dependent and now waits past its timeout.
- **Written, building:**
  - cooked format (`serialization/CookStream`, `CookCodecs`, `CookedScene`,
    `SceneSettingsCodec`, `codecs/`), `RuntimeFields` + the emitter
    (`jasperengine-editor reflect --emit-runtime-fields`), `editor/cook/SceneCooker`,
    `editor/cook/JPakWriter`, `tools/jpak.py`;
  - the player (`engine/player/PlayerMain.cpp`, `-DJASPERENGINE_PLAYER=ON`, ids
    `<platform>-<arch>-<backend>-<flavour>`), `player/PlayerConfig` (game.json),
    `player/ContentManifest` (R21 checks);
  - Exporter `ExportMode::Player` (`editor/ExporterPrebuilt.cpp`): method choice (R5; Auto
    compiles when the runtime is missing and this computer can), the R6 steps, staging swap
    (R7), finalizers for Linux, Windows (resource patching), macOS (bundle, ICNS, codesign /
    rcodesign), web (template, loader, `_headers`), Android (aapt2/zipalign/apksigner, AAB via
    bundletool, no NDK/Gradle), iOS (Xcode template; xcodebuild on a Mac), the payload
    validator before the swap;
  - player runtimes (`editor/players/PlayerRuntimes`: list, find with the version lock,
    install with the hash table, download with progress and cancel, remove), miniz 3.0.2
    (`libs/miniz`, `editor/util/ZipArchive`);
  - publish (`editor/publish/`: `BinaryInfo`, `TreeManifest`, `PayloadValidator`,
    `Publishing`: publishing.json, prepare with symbol split, verify, SteamPipe scripts,
    steam-upload, Deck deploy/rollback/smoke), notices (`engine/notices.json`,
    `editor/publish/Notices`, `tools/ci/check-notices.py --player-notices/--third-party-notices`);
  - CLI `build`, `players`, `publish`, `reflect`; MCP: `build_project` mode `player` (+
    `platform`, `method`, `development`, `android_format`), `get_build_info` (`players[]`,
    `platforms.<name>.{method, compile_reasons, tools_missing}`, `editor_version`), `run_build`
    (`headless`, `quit_after`, `fixed_fps`, `after_ready`, `screenshot_frame`: the player's own
    frame), the `shipping` group (`install_player_runtime`, `publish_prepare`,
    `publish_verify`, `publish_steam_vdf`, `publish_deploy_deck`); guide line +
    `docs/mcp/guide/shipping.md`;
  - UI: Build Settings (every platform builds; Build Method row and override; Player Runtime
    row with Download/progress/cancel; Android package/keystore/tool checklist and the password
    prompt; iOS "Requires a Mac with Xcode"; Build And Run on the host), Publish tab (targets
    table, Prepare, Verify, Steam VDF, Upload..., Deploy to Deck, Rollback as cancellable jobs
    logging to the Console), Preferences > External Tools > Shipping (Android SDK, JDK,
    SteamCMD, rcodesign, Player Runtimes list/Remove/Install from file), Project Settings rows
    and SettingsRegistry keys (`player.build_method`, `player.pack_content`,
    `android.package_format`, `android.keystore_{path,alias}`, `ios.team_id`,
    `ios.export_method`, `preferences.{android_sdk,jdk,steamcmd,rcodesign}_path`);
  - web: `engine/platform/emscripten/web/` (`player.html`, `player-loader.js` deciding once,
    `coi-serviceworker.js`), shared with the Compile web build (S06 AC21 fix);
  - Android player variant (`-PjasperPlayer=true [-PjasperBackend=vulkan]`,
    `tools/players/android_player.sh`), iOS (`engine/platform/apple/ios/player-template`,
    `tools/players/ios_player.sh`), `tools/players/package_player.py`, `write_hashes.py`,
    `tools/build.sh players` (linux, web, android) and `exec runtime`, `docker/runtime.Dockerfile`,
    `tools/smoke-test.sh` `SMOKE_METHOD`, `tools/ci/test_player_cli.py`, web gates
    `prebuilt-unified-lighting*.json`, CI jobs (`players-*`, `player-hashes`, `prebuilt`);
  - tests: editor `CookedScene.*`, `RuntimeFields.Generated`, `PayloadValidator.*`,
    `Publishing.*`, `PublishSteam.*`, `PublishDeck.*`, `PlayerInstall.*`, `BuildMethod.*`; MCP
    `test_build_method.py`, `test_publish.py`.
- **Verified (remote servers):**
  - Linux players glcore / gles3 / vulkan, built with warnings as errors. They build the fixture
    in the compiler-free image (`docker/runtime.Dockerfile`) and run it headless. Each matches
    its golden and repeats exactly. The contexts it reports:
    - glcore: `4.5 (Core Profile) Mesa`;
    - gles3: `OpenGL ES 3.2 Mesa`, and only gles3 links `libGLESv2`;
    - vulkan: `Vulkan 1.4.318`.
  - Web: all four modes pass with a checked frame. The modes are threaded, single-thread, the
    COI worker, and no isolation falling back to single-thread (S06 AC20/AC21).
  - Android: the prebuilt-path development APK passes the emulator check
    (`tools/ci/android_prebuilt.sh`).
  - The `xcode_sync_*.py --check` scripts pass.
- **S18 (scene format 5):** spline fields are S08 fields with a `cppPath` through `SplineData`'s
  editable accessors. The emitter now writes those as `JE_RF_ACCESSOR` + `JE_RF_PATH`
  (`writePath` / `readPath` in `CookCodecs.h`). A `spline` afterLoad hook keeps the decode's
  invariants (radii, the unit up axis) and bumps the curve's version. Covered by the
  `CookedScene.SplineCurveThroughAccessors` test.
- **Merges:**
  - oe-port 40bec8043 (BF8) is merged. The guide keeps one line for shipping
    (`build_project: ...; ship: jasper://docs/shipping.`). That line replaces the old Export
    sentence and is one byte shorter, so the headroom check still passes.
  - `RuntimeFields.gen.inc` was regenerated after the S18 merge and now holds 77 components.
- **oe-port e3f0c274d (S32 async shaders) merged:**
  - Conflicts were resolved keeping both sides. `Engine.cpp` gets S32's shader warm-up beside
    S30's `Graphics:` line.
  - The Mac and Windows platforms create S32's GPU worker context, with the vsync of S30's
    platform configuration.
  - The Xcode project is the union of both, and every `xcode_sync_*.py --check` passes.
  - Prebuilt builds ship S32's warm-up list too: `buildAndSaveShaders` writes
    `shaders/warmup.json`, and the staging copies it into the content.
  - A player's `--screenshot` now waits for S32's pending GPU creations, as the editor's
    screenshots do.
    - The game is held, the frames drawn while it waits are not counted, and the quit follows
      the capture.
    - It waits at most 30 s, then captures what is drawn with a warning.
    - Covered by `RunControlFixture.ScreenshotWaitsForPendingGpuCreation`.
- **Fixes found by the runs:**
  - S18's OpenEngine notice for `core/spline` was copied from the export's engine copy, and a
    prebuilt build has none. It now comes from `Exporter::engineSourceDir()`: the export's copy,
    else the editor's engine. Caught by MCP `test_prebuilt_linux_build` and `test_release_flow`.
  - `writeSteamScripts` kept a caller's earlier error message, which failed the next upload
    (`PublishSteam.UploadRunsSteamcmdAfterVerify`).
- **Shipped size (in progress, the coordinator's request):**
  - Release games now link with `JASPERENGINE_RELEASE_LINK` (engine/CMakeLists.txt): sections and
    `--gc-sections`, hidden visibility, the Android export list
    (`platform/android/exports.map`), and ThinLTO plus `--icf=safe` with lld.
  - The numbers: `tools/ci/android_size.sh` (APKs, prebuilt and compiled) and
    `tools/players/lib_size.py` (the library by source directory).
  - `test_player_cli.py` gained a `components` check: every component type goes through the
    release player.
  - The Android payload check now asks 16 KB pages of the 64-bit ABIs only.
- **Landing gate v2:**
  - Android part on A (bb4389942), all green:
    - the editor, the release players of every ABI, and the development x86_64 runtime;
    - the sizes, and both emulator checks with `--relaunch`;
    - AC7, and `tools/build.sh android`.
  - Everything else on D (cac2f837a), all green except as noted:
    - the xcode checks, the editor with warnings as errors, and TOOLS.md current;
    - the Linux and web players, and the web build with warnings as errors;
    - units: GL 2382 core and 1413 editor, Vulkan 2380 and 1413 with none skipped;
    - ASan + UBSan, the full suites;
    - the player CLI (the components check and the four web modes), and both smokes;
    - MCP, the full suite with slow builds: 1056 passed and 3 failed.
      - `test_desktop_build_and_run` failed because the compile fallback finished the build
        before its executable was known. Fixed in 9d854d51e (`Exporter::holdFinish`).
      - `test_reflection` golden_resave and codegen_golden are known oe-port failures
        (fix/s32-golden-determinism).
- **oe-port dc6eb4f2d (SYNC#3, vertex animation) merged:**
  - `RenderSystem.cpp` keeps upstream's `mesh.loadFailed` and S30's settled empty mesh. The VAT
    adds no new kind of pending load.
  - `RuntimeFields.gen.inc` was regenerated and adds the Vertex Animator; its attachments'
    entities are remapped. It needs no afterLoad hook: the runtime already takes any fade time
    from Lua.
  - The short re-check on D (06e2a63dc) is all green:
    - the xcode checks, the editor with warnings as errors, TOOLS.md and the runtime tables
      current, and the Linux players with warnings as errors;
    - GL units 2433 core and 1645 editor, including `LoadSettledGpu`,
      `LuaFunctionLifetime` and `RunControlFixture`;
    - the player CLI (the goldens, every component type including the Vertex Animator, the four
      web modes), and both smokes;
    - MCP: the export, cook, build, publish and VAT files with slow builds, 156 passed and
      0 failed. That includes `test_desktop_build_and_run`.

### Shipped size (measured)

- **The build:** `tools/fixtures/unified-lighting`, release, gles3.
- **How it was measured:**
  - Android: `tools/ci/android_size.sh` on server A, on code at 1c94d68a0 with the
    release-link fixes.
  - The size breakdown: `tools/players/lib_size.py`.
  - Linux: `.s30-linux-size.sh` on server B.
- **Units:** MiB (`apk_size.py`).
- **"Before"** is the same code without `JASPERENGINE_RELEASE_LINK`, with every APK's libraries
  stored.
- **"After"** has:
  - the release link: sections and `--gc-sections`, hidden visibility with the Android export
    list, ThinLTO and `--icf=safe`;
  - libraries packed as the Android Gradle plugin packs them: compressed below minSdk 23, and the
    fixture's minSdk is 21.

**Android engine library**, stripped, uncompressed (MiB; its split symbols in brackets):

| ABI | before | after | final gate |
|---|---|---|---|
| arm64-v8a | 39.46 (497) | 27.06 (390) | 27.71 (405) |
| armeabi-v7a | 28.99 (413) | 18.99 (338) | 19.42 (351) |
| x86 | 41.86 (408) | 33.16 (378) | 33.98 (392) |
| x86_64 | 42.04 (478) | 33.25 (422) | 34.04 (437) |

The final gate ran on bb4389942, which adds S21's Animator, BF7 and S32.

- `libc++_shared.so` adds 0.81 to 1.20 per ABI.
- Exported dynamic symbols: 44,529 before, 6 after. Those are `GameActivity_onCreate` and the
  five `Java_*` entry points.

**Android APKs** (MiB):

| package | before | after | final gate |
|---|---|---|---|
| prebuilt arm64-v8a APK | 41.61 | 12.45 | 12.74 |
| prebuilt armeabi-v7a APK | 30.76 | 11.38 | 11.64 |
| prebuilt x86 / x86_64 APK | — | 14.27 / 14.18 | 14.60 / 14.50 |
| prebuilt universal APK (4 ABIs) | 157.61 | 49.45 | 50.64 |
| compiled arm64-v8a APK (Gradle, the same project) | 15.13 | 12.31 | 12.59 |

- **Google Play download per device** (after): the App Bundle, the default, measured with
  `bundletool get-size total --dimensions=ABI` in the final gate. The `.aab` uploaded is 50.54 MiB.

  | ABI | download (MiB) | its per-ABI APK (MiB) |
  |---|---|---|
  | arm64-v8a | 12.40 to 12.42 | 12.74 |
  | armeabi-v7a | 11.28 to 11.30 | 11.64 |
  | x86 | 14.25 to 14.27 | 14.60 |
  | x86_64 | 14.15 to 14.17 | 14.50 |

- Inside an APK (after), each package holds:
  - its native libraries: 11.50 for arm64-v8a, compressed from 28.26;
  - dex 0.34 (0.76 unpacked) and resources 0.39;
  - content 0.10 and the signature 0.03.
- Prebuilt and compiled now cost the same. In the final gate the compiled library is 28,904,736
  bytes against the prebuilt's 29,059,056. The 0.14 MiB APK difference is the prebuilt's v1 signature and its content
  files.
- Before, the gap came from packing: the prebuilt stored its libraries, while Gradle compressed
  them. The libraries themselves were the same size by both methods (40,597,584 bytes compiled,
  41,374,936 prebuilt).
- AndroidX (appcompat, core, activity) costs about 0.7 MiB of dex and resources. GameActivity
  extends AppCompatActivity, so it stays, and the theme is not its only use.

**Linux** (glcore, MiB):
- "After" has the release link (sections and `--gc-sections`, hidden visibility; GCC builds have
  no LTO) and `JASPERENGINE_SPLIT_SYMBOLS`.
- It also includes S21's Animator, which "before" did not.

| build | before | after |
|---|---|---|
| prebuilt: the player | 43.88 stripped (744 symbols) | 40.77 stripped (767 symbols) |
| prebuilt: the whole build | 44.15 | 41.04 |
| compiled: the executable | 50.99 unstripped | 39.57 stripped (739 symbols) |
| compiled: the whole build | 51.26 | 39.84 |

- Prebuilt and compiled are now within 1.2 MiB of each other. The prebuilt player has every
  feature in it.
- Where the symbols go:
  - publish prepare copies the player's into `symbols/linux/player`;
  - a compiled build keeps its own under `.jasperengine/symbols/<sha256>/`, and its content
    manifest names them, for `symbols/linux/game`.

**What is left, by size.** These need per-project module stripping, which is the coordinator's
follow-up stream, not S30.
- The figures are symbol sizes of the arm64-v8a library after the release link, from
  `lib_size.py` (24.8 MB of named code and data in total). Rodata and unwind tables add roughly
  10% on top.
- Savings would be per build that leaves a feature out.

| module | named size (MB) |
|---|---|
| HTML UI: rmlui 1.74, engine 1.55, lexbor 0.70, thorvg 0.19, lunasvg 0.13 | 4.3 |
| HTML UI video: dav1d 0.44, opus 0.09, libwebm 0.06, engine 0.08 | 0.67 |
| runtime shader compiler: glslang 0.87, spirv-cross 0.55 | 1.42 |
| 3D physics: Jolt 0.73, plus engine physics 0.22 shared with 2D | 0.95 |
| 2D physics: box2d | 0.35 |
| navigation: recast 0.15, engine 0.20 | 0.35 |
| particles / splines / lattice / fluid / terrain | 0.28 / 0.24 / 0.05 / 0.05 / 0.01 |
| Lua bindings (luabridge3 templates) | 1.68 |

- The runtime shader compiler is only needed for variants the build did not precompile.
- The Lua bindings could shrink with fewer instantiations or `-Oz` on the binding sources.
- `-Oz` for the whole library would typically take another 15 to 25% of code. It needs a speed
  check first.
- A Vulkan backend is not in a gles3 player.
- About 3.3 MB is file-local helpers that the name grouping cannot attribute.

### Integrator notes

- `tools/web/browser_gate.py` now **fails** a run that asked for `--screenshot` when the page gave
  none (`Page.captureScreenshot` timed out or returned nothing). Before, it skipped the image
  silently, and a baseline without `rgb` passed without any frame being checked. Gates of other
  streams may start failing honestly once this lands. A page that genuinely cannot be captured
  opts out with `--allow-no-capture "<reason>"`; a heavy scene on software WebGL raises the wait
  with `--capture-timeout` or a baseline's `capture_timeout` (default 30 s).
- Why the prebuilt web frame was not captured: on the headless shell's SwiftShader WebGL the
  unified-lighting scene draws at about 1.5 fps, and the capture waits for the GPU process to
  produce a frame. The S30 baselines run the player to a fixed end (`?quit-after=3&fixed-fps=60&after-ready`,
  expecting `JasperEngine exit code 0`) and allow 300 s for the capture; the frame is then
  checked (centre pixel, and the player CLI test refuses a one-colour page).
- The engine logs one `Graphics: <backend>; <API version>; <device>` line when its view loads
  (e.g. `OpenGL ES 3 / WebGL2; OpenGL ES 3.2 Mesa 25.2.8; llvmpipe`).
- `--after-ready` together with `--fixed-fps` holds the game's update events and clocks until the
  ready frame (loading and drawing go on), so a run's frames repeat exactly.

- `check`'s summary aborted with `map::at` on every non-quiet `check --shaders` run (S19's
  `navigation` category had no unit). Fixed in 5ed9c9038. BF7 cherry-picks it with a regression
  test, so the next oe-port merge takes it cleanly.
- The OpenEngine notice of `core/spline` (S18) is copied from `Exporter::engineSourceDir()`. That
  is the export's engine copy, or the editor's own engine dir in a prebuilt build.
- These MCP tests fail on oe-port at 40bec8043 itself, not because of S30:
  - `test_pose_runtime` ×7 and `test_post_fx_golden` (goldens, defaults_unchanged);
  - `test_shadows` (cascade_blend_fade, shadow_goldens);
  - `test_lattice` show_all, `test_lattice_anim` preview, `test_scene_inputs` manifest_rules.

  On server D they fail the same way with S30's editor and with BF7's oe-port-based editor, both
  run against S30's test files. S30's run options (offscreen screenshot frames, `--after-ready`)
  take effect only in a player that `player::launch` configured, and the editor never calls it.

  Corrected attribution (the S18 fix agent: they also fail on the pre-S18 baseline):
  - BF7's branch fixes all of them except `test_pose_runtime`, mostly in the tests themselves,
    which is why BF7's editor still failed them against S30's test files:
    - `test_post_fx_golden`: S14's mip chain;
    - `test_shadows` and `test_lattice` goldens: S26's Scene camera field of view;
    - `test_scene_inputs`: S20's message;
    - `test_shader_library` and `test_synthesize_texture`: the check summary's `map::at`;
    - `test_place_physics_cli`: S16's HTML UI settings in project.yaml.
  - `test_pose_runtime` comes from S29's R20, which S29 is fixing.

  S30's final gate counts these files as known oe-port failures while oe-port still fails them,
  and requires everything else green.
- `SokolGlTextureLevels.OneLevelTexturesStateTheirMaxLevel` (BF3, 13e13a552) is GL-only. Under
  `GRAPHIC_BACKEND=vulkan` it called `GTEST_SKIP`, and the unit gate (`--max-skipped 0`) failed
  oe-port's Vulkan core run. S30 compiles it only for GL instead.
- An engine restarted in the same process called Lua functions of the closed state. This happens
  when Android relaunches an activity for a configuration change its manifest does not handle;
  `0x80000000`, right after an install, is one an app cannot declare.
  - `Engine::systemShutdown` closes Lua before anything releases the scripts' engine-event
    subscriptions, so `Engine.onUpdate` kept the old `LuaFunction`s.
  - The next instance called them, and crashed with SIGSEGV in `lua_rawgeti`. The S30 emulator
    check of a release APK caught it.
  - Each Lua state now has a generation (`LuaBinding::getLuaStateGeneration`). A `LuaFunction` of
    an older state is never called nor released. Covered by `LuaFunctionLifetime.*`.
  - The old subscriptions stay as inert entries. Releasing every script's subscriptions at shutdown
    would remove them too; that is a follow-up for the engine-lifecycle owner.
  - The emulator check now forces such a relaunch: `--relaunch` changes the font scale. The run
    needs a new view, the same process, and no crash. With the fix, the development prebuilt APK
    passed it (relaunched in process 1914, still running).
- A scene with an asset it cannot load used to never get ready.
  - `RenderSystem::isAllLoaded()` counted a mesh with no geometry as pending forever:
    `loadMesh` returns early on empty buffers and never sets `loadCalled`.
  - A model whose file is missing leaves exactly such a mesh. So does the reflection fixture's
    empty `MeshComponent`, which is what kept `test_player_cli`'s components run from "Engine
    ready".
  - A game still played, and `SceneManager`'s loading screen moves on when progress stalls. But an
    `--after-ready` run waited for its `--ready-timeout` and exited 6.
  - Now a mesh with nothing to load counts as settled: no buffers, and no model or its load over.
    A model still loading is still waited for. The failure stays logged ("Model load failed, not
    retrying").
  - The ready timeout's error names what it waited for (`Engine::describeNotLoaded`).
  - Covered by `LoadSettledGpu.*`, and the components check runs `--after-ready` again.
- Red before green, on server D, for both fixes. With the fix taken out, each test fails:
  - `LoadSettledGpu` still waits for entities 1002 and 1003 (mesh);
  - `LuaFunctionLifetime` still counts the closed state's function as live.

  With the fixes back, both pass.


### macOS runtime validation (BF4, 2026-10-31)

On macOS 26.2 (Apple silicon, Xcode 26.6): the universal Metal player builds with
`JASPERENGINE_WERROR` as the players-apple job builds it, is signed ad hoc, packaged
(`package_player.py --platform macos --arch universal`), and a prebuilt export of
`tools/fixtures/unified-lighting` stages, re-signs (adhoc, hardened runtime) and runs
`--headless --after-ready --quit-after 30 --screenshot` (2560x1440, exit 0 in 2.3 s; the lid was
closed, so nothing was on a display). Two defects were fixed first: the player was a bare
executable under Ninja (no `jasperengine-player.app` for the job, the packager or the exporter's
template), and a quit asked for inside a frame deadlocked in `applicationWillTerminate:` (the
headless run never exited). `LSMinimumSystemVersion` of an export is 12.0, the binaries' target.
Left: dsymutil skips liblexbor.a members with duplicate names (`tag.c.o`, `error.c.o`), so those
lexbor objects have no debug information in the player's dSYM; the glcore macOS runtime and the
iOS xcframework template were not run.
