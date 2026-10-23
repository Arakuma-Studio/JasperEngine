# 10 — Platform, Build, Export, Web, Tooling, Tests, CI, Docs & Hygiene

OURS = JasperEngine (`/var/lib/orquester/workspaces/jaspersito/JasperEngine-2`), THEIRS = OpenEngine (`external/OpenEngine`, MIT, `VERSION` = `2026.10.0-alpha.6`).
Everything below was checked against code and config, not file names. Paths are relative to each repo root.

**Headline facts:**
- THEIRS has **no CI at all**. `.github/` holds only `CODEOWNERS` and `PULL_REQUEST_TEMPLATE.md`, and the repo has no workflow YAML of any kind. Gating is manual or agent-driven, and the rules for it live in a private `GameEngine-Workbench` repo (`CONTRIBUTING.md`).
- THEIRS has **no Android and no iOS**. Root `CMakeLists.txt` has 0 `ANDROID` hits, and `Engine/` and `Apps/` have no `__ANDROID__` or `TARGET_OS_IPHONE`. Its targets are Win x64/ARM64, macOS, Linux, Steam Deck (Linux x64 on the SteamRT4 ABI), and wasm (WebGPU only).
- THEIRS has **no real Steamworks integration**. `PlatformServices` is an ABI with exactly one provider, `PlatformProviders/Local`, and `steam_api`/`SteamAPI_Init` appear nowhere. "Steam support" means SteamPipe upload scripting (`Tools/Scripts/publish_game.py`) plus the Steam Deck build and deploy flow.
- OURS builds 6 targets in CI on every push or on demand, and ships release assets (AppImage+zsync, dmg, zips). We have the better CI and the wider export reach. THEIRS has a much larger test surface (about 450k lines of tests) and a far more mature build system.

---

## 1. What THEIRS has

### Dependency management
- **vcpkg manifest mode**: `vcpkg.json` has about 40 ports with platform qualifiers and manifest **features** (`video`/ffmpeg, `physics`/jolt, `nav`/recast, `ui-extras`/lexbor+thorvg, `tests`/gtest+benchmark, `docs`). With a feature off, the module compiles as a stub (`find_package(... QUIET)`).
- `vcpkg-configuration.json` pins the registry baseline. `cmake/VcpkgPin.cmake` separately pins the vcpkg *tool checkout* (`GE_VCPKG_COMMIT`) because the binary-cache ABI hash covers the tooling, and its long comment explains why.
- Bootstrap: the first configure clones vcpkg into `dependencies/vcpkg/`. A per-user binary cache is shared by every clone, and `Tools/Scripts/fetch-vcpkg-cache.{sh,ps1}` fetches prebuilt archives (through `gh` when logged in, opt out with `GE_NO_CACHE_FETCH`).
- Overlay ports: `cmake/ports/` (about 22, e.g. glfw3, joltphysics, ktx, wgpu-native, shaderc, yoga), `cmake/macos-ports/`, `cmake/steamdeck-ports/openssl`.
  - `cmake/ports/README.md` has one rule: every behavioural edit bumps `port-version`.
  - `cmake/OverlayPortGuard.cmake` hard-fails a configure on a stale install (escape hatch `GE_ALLOW_STALE_OVERLAY_PORTS`). `cmake/VcpkgInstallGuard.cmake` refuses shared `vcpkg_installed` trees.
- Custom triplets in `cmake/triplets/`: `x64-steamdeck.cmake` (no OpenSSL asm, optional cross-compile), `wasm32-emscripten{,-nothreads}.cmake`. Toolchains are in `cmake/triplets/toolchains/`.
- The emsdk pin is `Tools/emsdk/setup.sh` (emsdk 6.0.5, idempotent, sourceable).

### Build system and speed
- `CMakePresets.json` (about 38 KB, ~45 configure presets) covers: vs2022/vs2026 x64 and arm64, `ninja-x64-local`, `ninja-x64-sccache{,-unity}`, `linux-x64{,-local}{,-unity}`, `steamdeck-x64{,-local}{,-unity}`, `steamdeck-runtime4`, `macos-{arm64,x64}-{local,ninja}`, `macos-arm64-editor-fast`, `-asan`/`-tsan`, and `wasm-{debug,release}{,-singlethread}`.
- **Unity builds** are opt-in per preset (`CMAKE_UNITY_BUILD=ON`). TUs that can't be unity-built carry `SKIP_UNITY_BUILD_INCLUSION` (`cmake/AllocationHook.cmake:37`).
- **sccache** presets exist. Root `CMakeLists.txt:143` selects `/Z7` embedded debug info so MSVC objects are cacheable. There are no explicit PCH (`target_precompile_headers` absent).
- **Custom `DebugFast` config** (`BUILD.md` "Build configurations"): `/MD`, no `/RTC1`, no static analysis, asserts live, warnings-as-errors. It is the default daily config.
  - MSVC uses Werror in Debug, DebugFast and RelWithDebInfo. `ENABLE_WERROR_NON_MSVC` defaults ON for GCC and Clang.
- **Sanitizers**: `ENABLE_ASAN` (MSVC `/fsanitize=address`, which strips `/RTC*` and stages the ASan runtime DLLs, `CMakeLists.txt:842-992`) and `ENABLE_TSAN`, each with presets.
- **No fuzzing**: no `LLVMFuzzerTestOneInput` exists anywhere.
- **CMake script unit tests**: `cmake/Tests/*.cmake` (23 files, e.g. `EngineVersionTest`, `VcpkgPinTest`, `StageThirdPartyNoticesTest`), registered through `cmake -P` in `Tests/CMakeLists.txt:199+`.
- The build is heavily modularised in `cmake/` (about 50 .cmake files): staging runtime DLLs and MoltenVK, mac bundle fixup/codesign/validate (`FixupMacAppBundle`, `CodesignMacAppBundle`, `ValidateMacAppBundle`), `WriteBuildCommit.cmake`, `ClonePathGuard.cmake` (refuses paths with spaces or that are too deep), and `EngineModuleDuplicationCheck.cmake`.
- Documented disk footprint is about 20–35 GB, and a cold configure compiles about 60 ports in tens of minutes (`BUILD.md`).

### Versioning and release
- `VERSION` holds a single line, `<year>.<month>.<patch>[-alpha.N|-beta.N]`. `cmake/EngineVersion.cmake` validates it with a regex and FATAL_ERRORs on a bad value.
- The editor title, mac bundle keys, managed assemblies and the npm package all read the same file. A release tag is `v`+VERSION, and `Tools/Web/build_web_package.py` refuses a tag that doesn't match.
- There is no CHANGELOG file. Each docs page has its own "changelog section" (`CONTRIBUTING.md`).

### Platform layer (`Engine/Modules/Platform`, about 15.9k lines)
- Windowing is GLFW (`Source/Window.cpp`, including the Emscripten GLFW port with documented quirks).
- Per-OS backends cover:
  - native file dialogs (`Shell.h`: `SelectFolder/SelectFile/SelectFiles/SaveFile`, `ShowNativeChoiceDialog`, `MoveToTrash`, `ShowInFileManager`, `LaunchDetached`, `IsProcessRunning`)
  - context menus, native toolbar, overlay window, system theme, system fonts, system metrics, clipboard, gamepad, HTTP client (curl, plus an `HttpClient_Emscripten.cpp`)
  - embedded **WebView** (WebView2 on Win, WKWebView on mac, a Linux version) and speech input (mac)
  - `Thread.cpp` (thread naming), `Process.cpp`
  - web: `WebPersistentStorage`, `WebEnvironment_Web`, `Shell_Web` (stubs that log "unavailable on web" and return failure)
- `Capabilities.h` has `SupportsDynamicNativeModules()`, `SupportsMultipleWindows()`, and a job-worker budget (0 on single-thread wasm).
- About 15 platform unit tests live in `Platform/Tests/`, e.g. `LinuxDialogPathsTests`, `HttpClientTests` against `TestSupport/LoopbackServer.h`, `Utf8ToWideTests`, `ThreadNameTests`.
- Crash handling:
  - The Editor writes full-memory **minidumps** on Windows (`Apps/Editor/Source/main.cpp:131`).
  - The Player installs `std::set_terminate`, `SetUnhandledExceptionFilter`, and SIGABRT/SIGSEGV handlers that only *log* (`Apps/Player/Source/main.cpp:40-80,325-332`).
  - `Engine/Modules/Logger/Source/Backtrace.cpp` provides symbolised backtraces for error logs and an editor hang watchdog.
  - There is no crash upload or breakpad/crashpad.

### Platform services (`Engine/Modules/PlatformServices`, about 1k lines)
- `PlatformProviderV1.h` is a **versioned C ABI** with `abiMajor/abiMinor/sizeBytes` and tail-append rules. It covers accounts, slot+file saves with `Commit`, achievements (unlock/progress/state), entitlements, platform UI (`ShowAchievements/ShowAccountPicker/ShowStorePage`), and a capability bitmask.
- `PlatformServices.h` is a C++ facade with thread-safety rules. The provider is chosen at configure time with `GE_PLATFORM_BACKEND` (`CMakeLists.txt:468`).
- Only `PlatformProviders/Local/Source/LocalProvider.cpp` (509 lines) exists: file saves plus mock achievements and entitlements. It is tested in `PlatformServices/Tests/PlatformServicesTests.cpp`.

### Image (`Engine/Modules/Image`, about 1.2k lines)
- `Decode/Encode/Resample/ImageHeader/PixelImage` over stb, with 4 test files. It is small and generic.

### Player and export
- `Apps/Player` is a prebuilt **runtime template**: a generic executable that reads `game.config` and `Assets/` staged beside it (or `Contents/Resources` on mac). The editor exports by staging content next to a prebuilt Player. A recompile happens only when the game has C#, C++ or native package modules (`docs/publishing/game-publishing.mdx`).
- Player CLI (`Apps/Player/Source/PlayerRunOptions.cpp`): `--headless`, `--quit-after N`, `--fixed-fps`, `--screenshot`, `--write-movie` (`PLAYER_MOVIE_RECORDER`), `--silent-audio`, `--hdr*`, `--config`, `--asset-root`, `--fullscreen`. This is what makes CI-style runs possible.
- `Tools/Scripts/publish_game.py` (about 32 KB, stdlib only) has `init/build/prepare/verify/preview/upload/deploy/rollback/smoke`, driven by a per-project `publishing.json`.
  - It checks the ELF/PE/Mach-O magic and arch of the Player and every `.so`.
  - It refuses `steam_appid.txt`, case-colliding paths, and non-relocatable symlinks.
  - It moves `.pdb`/`.debug` into `symbols/`, writes a `release.json` manifest of sha256 values, and re-verifies before upload.
  - It generates SteamPipe `app_build`/`depot_build` VDFs, refuses an auto `SetLive` on `default`/`public`, and deploys to a Steam Deck over SSH with rollback.
- Steam Deck: `Tools/Docker/Dockerfile.steamdeck-dev` builds from the **digest-pinned `registry.gitlab.steamos.cloud/steamrt/steamrt4/sdk`**.
  - `Tools/Scripts/build-steamdeck-docker.sh` (21 KB), `build_deck.py`, and `deploy-steamdeck.sh` handle the build and deploy.
  - `steamdeck-launch.sh` sets up the SSH-launch session env and the gamescope WSI bypass.
  - `package-shaderpkgs-on-deck.sh` packages shader packs on the device.

### Web (`Apps/WebLibrary`, `Tools/Web`)
- Renderer: **WebGPU** (emdawnwebgpu on web, wgpu-native on desktop). Shaders are cooked to WGSL (`Tools/Web/builtin_shader_cook.py`, `Tools/MaterialVariantCook/WebWgslCook.cpp`, pinned naga and tint).
- Two ABIs: threaded (pthreads + SharedArrayBuffer) and single-threaded (`wasm32-emscripten-nothreads`, JobSystem inline).
  - `Engine.create({threads:'auto'})` picks the threaded one only when the page is cross-origin isolated.
  - `coi-serviceworker.js` (MIT, Guido Zuidhof) supplies COOP/COEP headers on hosts like GitHub Pages.
- **npm package** `@game-crafters-guild/openengine-web` (alpha, not published yet). It is a TypeScript API (`ts/src`, 25 files, plus `ts/test`) over a C ABI (`ge_create/ge_tick/ge_entity_*/ge_component_*/ge_query_*`, `Apps/WebLibrary/CMakeLists.txt`).
  - It ships `ASYNCIFY`, `MODULARIZE`, `EXPORT_ES6`, `llms.txt`, examples, and a gallery.
- **Editor in the browser**: the `WebEditor` target (`Apps/Editor/CMakeLists.txt:1017-1140`).
  - It compiles the whole editor into one static wasm on a single canvas.
  - It is refused in Debug, because Yoga layout blows the ~1 MB browser stack.
  - Shaders come from a desktop build's `Shaders/`, with a staleness and foreign-checkout guard.
- **Content packing**: `.gepak` (`Tools/Web/gepak.py`, reader `Apps/Player/Source/WebPackReader.cpp`). It is a trivial index+blob container with FNV-1a integrity and `..`/absolute-path rejection.
  - `export_web_player.py` produces a dist from a **project-agnostic wasm template** plus `player-manifest.json`, `content.gepak` and `shaders.gepak`. There is no per-project wasm relink.
- `Tools/Web/serve.py` is a dev server with COOP/COEP, the wasm MIME type, and `no-store`.
- `Tools/Web/browser_gate.py` (28 KB, stdlib only) drives headless Chrome over a hand-rolled CDP websocket.
  - It returns a timestamped console transcript, WebGPU validation errors, a `Page.captureScreenshot` capture, and the centre-pixel colour.
  - Its docstring notes that canvas readback via drawImage returns zeros and is a broken probe.
  - `web_package_gate.py` (133 KB) gates the npm package.
- About 13 Python `test_*.py` files sit beside the web tools and are registered in ctest (`Tests/Wasm/CMakeLists.txt:9-30`). Wasm runtime smokes are in `Tests/Wasm/*.cpp` and `WebLibraryAbiSmoke.mjs` (node).

### Tests
- Size: `Tests/` is about 224k lines and `Engine/Modules/*/Tests` about 226k lines, about 450k in total. There are 363 test executables (`add_executable(*Test*)`) and 444 `add_test`/`gtest_discover_tests` calls.
  - The biggest areas are Editor (76k), Assets (35k), Jobs (17k), ECS (16k) and NativeScripting (10k).
- Frameworks: GoogleTest everywhere (51 CMakeLists link GTest). Google Benchmark is in vcpkg but **unused**: there are 0 `BENCHMARK(` or `benchmark::` hits. Benchmarks are plain chrono executables.
- Other test types:
  - Python unittest for tools, node `.mjs` tests, CMake `-P` script tests, PowerShell runners.
  - Golden *data* tests: `Noise/Tests/NoiseGoldenTests.cpp`, `CBTTerrain/Tests/CBTDecodeGoldenTests.cpp`.
  - **GPU readback tests** that skip without a Vulkan device (`Tests/Rendering/MaterialLitCubeTests.cpp`, `Rendering/Tests/*` with `GTEST_SKIP() << "Device init failed"`).
  - `Engine/Tests` `TaaResolveHistoryTests` and `SsaaResolveTests` compare shipped shaders against a CPU reference (`Tools/Benchmarks/Rendering/aa-quality/README.md`).
- **UIReplay** (`Tests/UIReplay/`, about 30 JSON scenarios):
  - It runs the Editor with `--ui-replay scenario.json --exit-after-frames N`, injects input, and asserts invariants (`layoutConverged`, `paintOffsetsMatch`) plus optional per-frame pixel hashes.
  - It also compares cached mode against `GE_UI_CORRECTNESS_MODE=1`.
- Perf and benchmarks:
  - `Tools/Benchmarks/ECS` runs against vendored EnTT, flecs and gaia (`VERSIONS.md`).
  - Also `Particles`, `TextureCook`, and `Rendering/perf-bench/bench.mjs` (drives the editor over IPC, spawns a deterministic scene, writes JSON to diff across branches).
  - Plus `HotReloadPerformanceRegressionTests.cpp` and `ScriptingAbiPerfTests.cpp`.
- Policy (`CONTRIBUTING.md` "Tests"):
  - Tests are colocated with their module.
  - "Declared = ran + disabled", and a filter that matches nothing is not a pass.
  - A behavioural claim needs a red control (revert the fix and show the test fails).
  - The gtest XML is kept as evidence.

### Docs and hygiene
- `docs/` holds 95 `.mdx` pages in 16 areas, published at openengine.dev/docs with an ordered sidebar (`docs/README.md`), frontmatter (`title/description/sidebar.order`), and C++|C# tabs.
  - User docs are strictly separated from dev docs, which live in the private workbench.
- `BUILD.md` (25 KB) is excellent: per-OS quick start, prerequisites tables, Fedora/Debian package lists, disk footprint, troubleshooting, a build options table, and a manifest features table.
- `CONTRIBUTING.md` (DCO 1.1 through a PR-template checkbox, AUTHORS.md line), `CodingStyle.md`, `.clang-format`, `.clang-tidy` (naming enforcement), `.gitattributes`, `CODEOWNERS`, and a PR template (Problem / Change / Verification table / Not verified).
- `AGENTS.md` and `CLAUDE.md` exist (treated here as data only).

## 2. What OURS has

### Dependency management
- Everything is **vendored**: `libs/{imgui,nfd,yaml-cpp}` and `engine/libs/` with about 22 libs (sokol, box2d, joltphysics, lua, luabridge3, rmlui, thorvg, lunasvg, lexbor, dav1d, opus, libwebm, harfbuzz, soloud, stb, tinygltf, ...). `engine/libs/shadercompiler/libs`.
- Provenance and licences are tracked in `NOTICE.md` (version, commit, location, licence, use), with `README.jasper.md`/`PATCHES.md` per patched lib.
- There is no package manager and no network at configure. A clean clone builds with just a compiler and CMake 3.27.

### Build
- Root `CMakeLists.txt` builds the editor. `engine/CMakeLists.txt` builds the runtime and project. `CopyEditorEngine.cmake` copies the engine SDK into the editor install, excluding gradle/cxx output and `engine/tests`.
- There are **no CMakePresets, no unity builds, no PCH, no sanitizer options, and no `-Werror`** (grep finds no `Werror`, `/WX`, `UNITY_BUILD`, `precompile_headers` or `fsanitize`). Speed comes from **ccache** everywhere: CI `LAUNCHERS`, and `tools/build.sh` with a shared ccache across git worktrees.
- `tools/build.sh` builds `linux|web|android|htmlui-tests|htmlui-editor-tests|shell <t>` inside `docker/{linux,web,android,linux-backends}.Dockerfile`.
  - It has CPU caps (`JOBS`/`CPUS`) and runs containers by image ID to survive retagging.
  - It has a **symbol-leak gate**: `nm -D libjasperengine.so` must export no `Rml::`, lexbor, dav1d, opus or mkvparser symbols.
- The Ubuntu 22.04 glibc floor is pinned. The images carry an eMudhra root distrust check (`docker/linux.Dockerfile`, `docker/web.Dockerfile`).
- Version comes from `git describe --tags --match 'v[0-9]*'` (`CMakeLists.txt:6-24`, CI "Set version"). There is no VERSION file. Tags: only `doriax-base`.

### CI (`.github/workflows/ci.yml`)
- Every push builds Linux (editor + engine, AppImage with a pinned and sha256-verified appimagetool, an AppRun glibc/libstdc++ shim, and zsync delta updates), Web (setup-emsdk 6.0.10) and Android (gradle).
- On demand, PRs, `v*` tags and releases add Windows MSVC (editor, plus engine on GL and Vulkan, Ninja+ccache) and macOS universal (editor, ad-hoc codesign, dmg) plus the Xcode macOS and **iOS simulator** builds.
- A release job attaches zips, AppImage, zsync and dmg.
- **CI runs no tests**: there is no test step, and neither `smoke-test.sh` nor `htmlui-tests` runs in CI.

### Platform layer (`engine/platform/`)
- Native, without GLFW: `win/` (Win32 + D3D11 context, gamepad, manifest), `linux/` (X11/Wayland, `GamepadDB`), `mac/` (GL/Metal/Vulkan views, NSTextInputClient), `apple/{ios,macos}` (Xcode app shells, GameController, **AdMob**), `android/` (NativeEngine), `emscripten/`, `common/` (VulkanContext, SDL gamepad mappings).
- The interface is `engine/core/System.h` (virtual): window, fullscreen, clipboard, IME rect, user-data key/value store, `syncFileSystem` (IDBFS), **AdMob** and the **CrazyGames SDK**.
- Editor file dialogs use `libs/nfd`. Crash handling is editor-only: `editor/util/CrashHandler.cpp` (signal handler, `backtrace_symbols_fd` to a file). **Exported games have no crash handler.**
- Backends: GL core, GLES3/WebGL2, Vulkan, Metal and D3D11, all through sokol-gfx.

### Export (`editor/Exporter.{h,cpp}`, 3.8k lines; `engine/jasperengine.py`)
- `ExportMode::{SourceCode, Desktop, Web}`. Every export **generates a standalone CMake project and compiles it** with the user's toolchain or emsdk (`configureBuild/runBuild/collectDesktopArtifacts/collectWebArtifacts`).
- Shader-key selection comes from the scenes. Options include `--preload-file` assets/lua for web, an experimental `resources.pak` (`packNativeResources`), a custom web shell and favicon, and Android Studio/Xcode workspaces (`engine/workspaces/{androidstudio,xcode}`).
- The CLI is `jasperengine-editor export|shaders|benchmark` (README).

### Web
- sokol GLES3 → **WebGL2** (`engine/CMakeLists.txt:181-247`). pthreads are optional and OFF (`EMSCRIPTEN_THREAD_SUPPORT`, pool 4). The editor does not run in the browser.

### Tests
- `engine/tests/htmlui/`: about 70 gtest files plus a harness. The **golden-image tests** (38+ PNGs in `golden/`, `harness/ImageCompare.h` with maxDiff and diff images) run headless on Xvfb with Mesa llvmpipe (GL) and lavapipe (Vulkan).
  - `JASPER_HTMLUI_UPDATE_GOLDEN=1` re-baselines and `JASPER_HTMLUI_WRITE_ACTUAL=1` keeps the actual images.
- `editor/htmlui/tests/`: importer tests. In total there are about 52k lines of C++ tests, gtest 1.11 borrowed from `libs/yaml-cpp/test`.
- `tools/mcp/tests/`: 64 Python files, about 23k lines, running against a live editor or a fake server (`tools/mcp/run_tests.py`, `--xvfb`, `htmlui_pixels.py`, `pngdiff.py`).
- `tools/smoke-test.sh`: an E2E pipeline of CLI export → build → run under Xvfb → screenshot game and editor → fail on logged errors. It covers OpenGL, Vulkan/lavapipe and GLES3, waits on a ready signal (`JASPERENGINE_READY_SIGNAL`), and supports multi-run scene selection.
- `tools/fixtures/*` holds 10 fixture projects (shader-library, shader-regressions, post-processing, prefab-runtime, ...). There are prefab Python tests in `tools/prefab-*.py`.
- Perf: `tools/performance/*.cpp` microbenchmarks with runners, `tools/htmlui-bench`, and `jasperengine-editor benchmark` for scene FPS.
- **Tests cover only HTML-UI, MCP and editor flows.** There are **no unit tests for the core ECS, renderer, physics, serialization or platform layer**, and no 3D-renderer golden images.

### Docs and hygiene
- `README.md`, `NOTICE.md`, `docs/{shaders,mcp,html-ui,unity-glossary.md,superpowers}`. `docs/html-ui/phase-*.md` are dev design records sitting in the user docs tree.
- There is **no** CONTRIBUTING, CodingStyle, `.clang-format`/`.clang-tidy`, PR template, CODEOWNERS, CHANGELOG, VERSION file or docs site.
- `.git-blame-ignore-revs` exists. Repo-root clutter: `editor.log`, `__pycache__`, and three `generate_*.py` at the root.

## 3. Gaps — THEIRS has, OURS lacks

| Feature | Their location | Value to us | Effort |
|---|---|---|---|
| Prebuilt runtime template (export without a compiler/emsdk on the user machine; content staged beside a generic player) | `Apps/Player`, `Tools/Web/export_web_player.py`, `docs/publishing/game-publishing.mdx` | **H** (Lua-only games are the common case; per-export CMake builds are slow and fragile) | L |
| Release prepare/verify/upload pipeline (sha256 manifest, binary arch checks, symbol split, SteamPipe VDF, Deck SSH deploy/rollback) | `Tools/Scripts/publish_game.py` | **H** | S–M (port script, adapt validator) |
| Tests run in CI | (THEIRS also lacks this; OURS has the infrastructure: `tools/build.sh htmlui-tests`, `smoke-test.sh`) | **H** | S |
| Core engine unit tests (ECS, serialization, math, platform) colocated with modules | `Engine/Modules/*/Tests` (about 226k lines) | **H** | L (ongoing) |
| Headless runtime CLI (`--headless --quit-after --fixed-fps --screenshot`) for deterministic CI runs | `Apps/Player/Source/PlayerRunOptions.cpp` | **H** | S–M |
| Sanitizer builds (ASan/TSan/UBSan options + presets) | `CMakeLists.txt:842-992,981`, presets `*-asan`/`*-tsan` | **H** | S |
| CMakePresets.json (standard configure/build entry points, IDE integration) | `CMakePresets.json` | M | S |
| Unity-build option | `linux-x64-unity`, `SKIP_UNITY_BUILD_INCLUSION` usage | M (cold builds; our vendored libs dominate) | S–M |
| Warnings-as-errors config (DebugFast-style) | `BUILD.md` "Build configurations" | M | M (clean up existing warnings first) |
| Platform services ABI (achievements/saves/entitlements/accounts, capability mask) | `Engine/Modules/PlatformServices/Include/PlatformServices/PlatformProviderV1.h` | M (needed before any Steam/console/mobile store integration; would also tidy AdMob/CrazyGames out of `System.h`) | M |
| Steam Deck target (SteamRT4 SDK container, launch script) | `Tools/Docker/Dockerfile.steamdeck-dev`, `Tools/Scripts/steamdeck-launch.sh`, `build-steamdeck-docker.sh` | M | S (we already have a Linux Docker build; add a sniper/steamrt4 image) |
| Crash handler in shipped games (and minidumps on Win) | `Apps/Player/Source/main.cpp:40-80`, `Apps/Editor/Source/main.cpp:131` | M | S (lift our own `editor/util/CrashHandler.cpp` into the runtime) |
| Packed web content container with integrity | `Tools/Web/gepak.py`, `Apps/Player/Source/WebPackReader.cpp` | M (we use emscripten `--preload-file`) | S–M |
| COOP/COEP serving (dev server + service worker for static hosts), auto thread/no-thread pick | `Tools/Web/serve.py`, `Apps/WebLibrary/coi-serviceworker.js` | M (unblocks pthreads web builds on itch/GitHub Pages) | S |
| Headless-Chrome browser gate (console transcript, screenshot, validation errors) | `Tools/Web/browser_gate.py` | **H** (we have no automated web run check) | S |
| UI replay harness (scripted input JSON, invariants, pixel hash) | `Tests/UIReplay/` | M (our MCP tests partly cover this) | M |
| Single VERSION file + validator, tag==version check | `VERSION`, `cmake/EngineVersion.cmake` | M | S |
| Third-party notices staged into shipped builds | `cmake/StageThirdPartyNotices.cmake` | M (licence compliance for exported games: dav1d/opus/libwebm/lexbor/...) | S–M |
| Contributor docs: CONTRIBUTING (DCO), CodingStyle, .clang-format/.clang-tidy, PR template, CODEOWNERS, AUTHORS | repo root, `.github/` | M | S |
| BUILD.md-grade build docs (prereq tables, troubleshooting, options table) | `BUILD.md` | M | S |
| Docs site structure (mdx, frontmatter, ordered sidebar, user/dev split) | `docs/README.md`, `docs/**.mdx` | M | M |
| Editor in the browser | `Apps/Editor/CMakeLists.txt:1017+` (`WebEditor`) | L (big effort; our ImGui editor could do it, but low payoff now) | XL |
| Embeddable web library / npm package with TS API | `Apps/WebLibrary`, `ts/` | L–M | L |
| WebGPU web backend | vcpkg `wgpu-native`, emdawnwebgpu, WGSL cook | M long-term (sokol already has a WGPU backend: the effort is shader cross-compile to WGSL plus testing) | L |
| ECS benchmark vs EnTT/flecs/gaia | `Tools/Benchmarks/ECS` | L | S–M |
| CMake script contract tests | `cmake/Tests/*.cmake` | L | S |
| Native platform conveniences (MoveToTrash, ShowInFileManager, system theme/fonts, HTTP client) | `Engine/Modules/Platform/Include/Platform/Shell.h` etc. | L–M (editor UX) | M |

## 4. Things THEIRS does better / things OURS does better

### THEIRS better (evidence-based)
1. **Shipping pipeline rigour.** `publish_game.py` refuses to ship many classes of broken release, and every check is a few lines of stdlib Python:
   - the wrong Player architecture (ELF `e_machine`, PE machine and Mach-O cputype parsed by hand)
   - a stray `steam_appid.txt`, case-colliding paths (Windows/mac FS), or symlinks that escape the tree
   - a missing `.assetmanifest` or shader packages
   - content changed after `prepare` (sha256 manifest re-verified)

   OURS stops at "copy artifacts to destination".
2. **Export without a toolchain.** A prebuilt Player or wasm template plus a staged content manifest, so artists can export. OURS always runs CMake and a compiler (`Exporter::configureBuild/runBuild`), even for pure-Lua projects.
3. **Test depth and colocation.** About 450k lines of tests, with every module carrying `Tests/`, and explicit anti-false-green rules (declared = ran + disabled, empty filter ≠ pass, red control). OURS's tests cluster around HTML-UI and MCP. Core ECS, scene serialization and renderer have zero unit tests.
4. **Build ergonomics.** Presets for every host and config, unity, sccache with `/Z7`, a DebugFast config, ASan/TSan, Werror, and guards that turn silent cache or ABI corruption into actionable FATAL_ERRORs (`VcpkgPin`, `OverlayPortGuard`, `VcpkgInstallGuard`, `ClonePathGuard`).
5. **Web deployment realism.** Threaded and single-thread builds with automatic selection, the COI service worker, `serve.py`, the `browser_gate.py` headless capture, and packed content (2 requests instead of N).
6. **Platform-services abstraction** done properly: a C ABI, versioned, capability-driven, with "branch on capabilities, never on provider identity". OURS hard-codes AdMob and CrazyGames methods into `System.h`.
7. **Docs hygiene.** One canonical page per topic, user/dev split, a site sidebar, a contributor policy, DCO, PR template.

### OURS better
1. **Export target breadth.** Windows, Linux, macOS, **Android (APK in CI), iOS (Xcode, simulator in CI)** and HTML5, across 5 graphics backends (GL, GLES3/WebGL2, Vulkan, Metal, D3D11). THEIRS has no mobile at all and is Vulkan or WebGPU only (macOS via MoltenVK).
2. **Real CI with release artifacts.** `.github/workflows/ci.yml` builds 6 targets, does cost-aware gating (Win/mac on demand), uses ccache per job, and attaches AppImage+zsync (delta updates), dmg and zips to releases. It pins appimagetool by sha256 and has an AppRun glibc-floor/libstdc++ shim. THEIRS has **no CI whatsoever**.
3. **Hermetic, offline, fast-to-bootstrap build.** Vendored deps, no 60-port cold build, no 20–35 GB footprint, and a clean Docker path (`tools/build.sh`) where the host needs only Docker. THEIRS's first configure takes "tens of minutes" and its Docker images only cover Linux dev and Deck.
4. **Headless GPU golden tests that actually run without hardware.** Xvfb with llvmpipe and lavapipe, both GL and Vulkan, with an update-golden workflow. THEIRS's GPU tests `GTEST_SKIP` when no Vulkan device exists, and its pixel checks are hash-based (UIReplay) or centre-pixel (browser_gate).
5. **E2E smoke test** across three backends (`tools/smoke-test.sh`): export via the CLI, build the exported project, run, screenshot, and fail on logged errors. THEIRS has no equivalent single script. Its pieces are scattered across `publish_game.py smoke` and the `run-*-iol-test` PowerShell scripts.
6. **Shared-library symbol-leak gate** (`tools/build.sh`, `nm -D` must not export RmlUi, lexbor or decoder symbols).
7. **Supply-chain care in images.** CA distrust checks, a pinned glibc floor, digest and sha checks on tools.
8. **Licence provenance in one table** (`NOTICE.md`: version, commit, path, licence, purpose), versus THEIRS's scattered `THIRD_PARTY_NOTICES.md`. THEIRS does better at *staging* notices into builds.

## 5. Copy/port candidates

All THEIRS code is MIT (`Copyright (c) 2026 Gamecrafters Guild Limited and contributors`). When we lift files we keep the header or attribution, and add an entry to `NOTICE.md` ("portions adapted from OpenEngine, MIT").

1. **`Tools/Scripts/publish_game.py`** → `tools/publish_game.py`
   - Dependencies: Python 3.10 stdlib only, SteamCMD optional, ssh/scp for the Deck.
   - Adaptation: **S–M**. Keep `tree_manifest`, `digest`, `binary_target`, `prepare`, `verify_release`, `steam_scripts`/`vdf_string`, `deploy`/`remote_action`.
   - Rewrite `validate_payload` for our layout: executable + `assets/`, `lua/`, shader blobs or `resources.pak`. Drop their `.assetmanifest`, `.shaderpkg`, C#/NativeAOT and packages.index checks.
   - Replace `editor_call`/`build_in_editor` (their MCP IPC) with `jasperengine-editor export --mode desktop`.
   - Highest value per effort in this report: it gives us release manifests, arch validation and Steam upload in about a day.
2. **`Tools/Web/browser_gate.py`** (plus `Tools/Web/serve.py`) → `tools/web/`
   - Dependencies: Python stdlib, Chrome/Chromium. Our `docker/linux.Dockerfile` would need `chromium` added for CI.
   - Adaptation: **S**. Change the "WebGPU validation error" patterns to WebGL/sokol console errors and point it at our exported `<app>.html`. The CDP client is self-contained.
   - Add a CI step in the `web` job: export a fixture with `tools/fixtures/*`, run the gate, fail on console errors or a black centre pixel. This closes our "web build compiles but never runs" gap.
3. **Headless run flags from `Apps/Player/Source/PlayerRunOptions.cpp`**: the `--headless`, `--quit-after N`, `--fixed-fps`, `--screenshot` semantics and validation messages.
   - Adaptation: **S–M**. Reimplement in `engine/platform/*/main.cpp` and `JasperEngine*` plus the core loop (fixed dt and frame counter). We cannot paste the code (it is tied to their GameConfig), so we copy the semantics.
   - It complements `JASPERENGINE_READY_SIGNAL`. `smoke-test.sh` could then use `--quit-after` and `--screenshot` instead of timed `import` screenshots, which makes runs deterministic.
4. **Sanitizer and preset scaffolding**: the `ENABLE_ASAN`/`ENABLE_TSAN` option pattern (`CMakeLists.txt:981-1000`, the MSVC RTC strip and DLL staging at `:842-876`) and a slim `CMakePresets.json` (linux-ninja, linux-asan, linux-unity, windows-ninja, macos-ninja, web-emscripten).
   - Dependencies: none. Adaptation: **S**. Write our own short presets rather than copy their 38 KB vcpkg-bound file.
   - Add a CI job that runs `htmlui-tests` under ASan+UBSan in the Linux image.
5. **`PlatformProviderV1.h` + `PlatformServices` facade + `LocalProvider`** → `engine/core/platformservices/`
   - Dependencies: none (C ABI, std).
   - Adaptation: **M**. Rename `GE_` to `JE_`, back saves with our `getUserDataPath()` and IDBFS `syncFileSystem()` on web, and expose it to Lua through LuaBridge.
   - Later providers: Steamworks (desktop), Game Center/Play Games, CrazyGames. AdMob and CrazyGames then move out of `System.h` behind capabilities. The ABI design (sizeBytes plus tail-append, sinks with no ownership crossing) is the part worth copying verbatim.
6. **`Tools/Docker/Dockerfile.steamdeck-dev` + `Tools/Scripts/steamdeck-launch.sh`** → `docker/steamdeck.Dockerfile`, `tools/build.sh steamdeck`.
   - Dependencies: the SteamRT4 SDK image (public, Valve). Drop their dotnet stage.
   - Adaptation: **S**. Our CMake already builds Linux GL and Vulkan. Ship the launch script inside exported Linux builds as an optional `launch.sh`.
7. **`Tools/Web/gepak.py` + `WebPackReader.cpp`** → a format for our web assets, or to replace the experimental `resources.pak`.
   - Dependencies: none. Adaptation: **S–M**. Our runtime needs a reader into MEMFS or direct `platformFopen` hooks.
   - It gives integrity (FNV-1a) and path-traversal rejection, and decouples content from the wasm. That decoupling is the precondition for candidate 8.
8. **Prebuilt-template export model** (concept from `export_web_player.py` and the desktop Player template): ship a prebuilt `jasperengine-player` per platform and backend inside the editor install, and export Lua-only projects by staging `assets/`, `lua/`, shaders and a manifest next to it. Fall back to the CMake export when C++ scripts exist.
   - Adaptation: **L**. It touches `Exporter`, `CopyEditorEngine.cmake`, CI (to build templates), and runtime discovery of shaders and the start scene from a config file instead of compile-time generation.
   - Highest *user* value of all, and it reuses our CI matrix.
9. **`Apps/WebLibrary/coi-serviceworker.js`** (third-party MIT, Guido Zuidhof; keep `coi-serviceworker.LICENSE`).
   - Adaptation: **S**. Include it optionally in web exports when `EMSCRIPTEN_THREAD_SUPPORT=ON` so threaded builds work on itch.io and GitHub Pages.
10. **`cmake/EngineVersion.cmake` + `VERSION`**: adopt only if we want versions independent of tags (S). Otherwise keep `git describe` and add a tag==version check in the release job.
11. **`cmake/StageThirdPartyNotices.cmake` (pattern)**: stage `NOTICE.md` plus the `engine/libs/*/LICENSE*` files into exported games (`licenses/`). The pattern is worth taking but the code is vcpkg-share specific, so we rewrite it (S).
12. **Contributor scaffolding**: `.github/PULL_REQUEST_TEMPLATE.md` (Problem/Change/Verification/Not verified + DCO checkbox), `.clang-format`, and a subset of `.clang-tidy` re-targeted to our naming (camelCase methods, unlike their PascalCase). Use `CONTRIBUTING.md` sections "Tests" and "Commit messages" as a template. Effort S.

## 6. Not worth copying (and why)
- **vcpkg migration, overlay ports and the guard machinery** (`vcpkg.json`, `cmake/Vcpkg*.cmake`, `cmake/ports/`): it exists to fight problems vcpkg creates. Our vendored, offline build bootstraps in minutes, not a 60-port cold build of 20–35 GB. It would also break our Android and iOS paths, which have no triplets in THEIRS.
- **WebEditor (editor in browser)**: XL effort. Their own CMake documents stack-overflow constraints, and it needs WebGPU plus a desktop build's shaders. Our users author on desktop, and the payoff is low.
- **WebGPU/WGSL cook toolchain** (`builtin_shader_cook.py`, naga/tint pins, `WebWgslCook.cpp`): it is tied to their Vulkan/SPIR-V shaderpkg format. If we move to WebGPU we should use sokol's WGPU backend and our own shadercompiler.
- **`Apps/WebLibrary` npm/TS package**: it targets an "engine as JS library" product we don't have. It is a large ABI and type surface with an unpublished package.
- **`Engine/Modules/Platform` wholesale**: it is GLFW-based with WebView2/WKWebView, speech input and native toolbars. We have native windowing per OS, including mobile, and would only cherry-pick ideas (MoveToTrash, ShowInFileManager).
- **`Engine/Modules/Image`**: a thin stb wrapper. We already vendor stb, and nothing in it is unique.
- **UIReplay as-is**: Windows/PowerShell runners and their retained-UI invariants are specific to their Yoga/UXML editor UI. Our MCP harness (`tools/mcp/`) already drives the editor. Copy the *idea* (pixel hash per frame, correctness-mode diff) only if we hit UI caching bugs.
- **Their gating process** (private workbench test maps, visual-critic agents, "ledger rows"): unreproducible without their private repo, and too heavy for our team size. The useful rules (declared = ran + disabled, red control) fit in a few lines of CONTRIBUTING.
- **Google Benchmark in the deps**: they declare it but use it nowhere, so there is no evidence it pays off. Our plain-chrono `tools/performance` is equivalent.
- **The DebugFast custom config**: it is mainly an MSVC debug-CRT workaround (`/MDd` cost). On our GCC/Clang-heavy CI, a `RelWithDebInfo` + asserts build serves the same purpose.
