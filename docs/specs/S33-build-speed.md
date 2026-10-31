# S33 — Build speed

Wave 3. Depends on S02 (presets, `JASPERENGINE_UNITY_BUILD`, WERROR, the unit-test targets,
`tools/build.sh`) and S30 (release link flags, prebuilt player runtimes and their archives).

## 1. Goal

Engine, editor and game builds become much faster, both clean builds and the edit-and-rebuild
loop, with zero loss of correctness: every test stays green, every configuration stays
warning-free under WERROR, and shipped binaries are unchanged except where they are deliberately
better. Every lever in this spec is justified by a measurement of section 3 and is measured again
after it lands (section 7, AC1). A game with C++ scripts compiles only its own code and links a
prebuilt engine, as S30's prebuilt players already avoid compiling the engine at all.

## 2. Scope

**In scope**
- Measurement tooling that anyone can re-run: `tools/perf/build-bench.sh` (clean, warm-cache,
  no-op and edit rebuilds of one build tree inside its build image) and `tools/perf/build_stats.py`
  (`.ninja_log` breakdowns, header fan-out from `ninja -t deps`, header parse costs, GCC
  `-ftime-report` of the slowest units).
- ccache configuration of `tools/build.sh`, `tools/smoke-test.sh` and CI so a new build directory
  or worktree hits the cache (base directory, debug-info directory hashing, PCH sloppiness).
- Vendored libraries: compile commands free of tree-specific definitions and paths, so one object
  serves every tree; unity builds of the vendored libraries.
- Unity builds of our own code where they pay off (CI and full builds), not in edit loops.
- Precompiled headers for our own targets.
- A faster linker for Linux development and test builds, with a fallback, never for shipped games.
- Header fan-out cuts where a one-line header change rebuilds hundreds of units.
- C++-script games (S30's Compile method) linking a prebuilt engine library.
- `docs/testing.md` (Build speed section) and this spec's measurements.

**Out of scope**
- vcpkg or any package manager (README do-not-copy list).
- C++20 modules (GCC 11 cannot build them reliably; MSVC/Clang/NDK/Emscripten parity missing).
- Distributed compilation (distcc, icecream) and a shared ccache across the build servers: see
  section 9 (follow-up), decided against for now.
- Changing optimisation levels, LTO or any code-generation flag of a shipped binary for speed.

## 3. Current state (measured)

The numbers are those of the section "Measurements" (M1–M8), oe-port b620283cd/e3f0c274d,
server F, 8 jobs.

- **Clean builds compile ~1.1 M lines four to six times.** The editor tree (10,140 compile CPU-s,
  1,296 s), the standalone engine (3,710 CPU-s, 482 s), the core tests (6,361 CPU-s, 840 s), the
  editor tests (24,998 CPU-s, 3,271 s), web (3,436 CPU-s, 447 s) and Android (four ABIs, 1,596 s)
  each compile the engine and every vendored library with their own flags (M1).
- **The editor-tests tree compiles every editor source twice**: once for `jasperengine-editor`
  (8,687 CPU-s) and once for `jasperengine-editor-tests` (9,078 CPU-s), "with the same command
  lines so ccache shares the objects" (`engine/cmake/JasperTesting.cmake:113-137`). In one build
  both compiles of a source run before either is cached, so nothing is shared (M1).
- **Vendored libraries are 18-42 % of every clean build** (M1): 1,356 CPU-s of the engine tree,
  1,810 of the editor, 1,428 of web, ~2,500 per Android ABI; RmlUi, Jolt and the shader compiler
  lead. Their compile commands carry the tree's directory-wide definitions (the editor's
  `JASPERENGINE_EDITOR`, `IMGUI_ENABLE_TEST_ENGINE`, ...; an export's window title, vsync and log
  settings, `engine/CMakeLists.txt:84-159`), so one tree's objects never serve another and every
  project setting change of an export recompiles all of them.
- **Our slowest units are code generation, not parsing** (M3): `-ftime-report` gives 55-75 % to
  "opt and generate" (InspectorWindow.cpp: 22.6 s parse, 55.7 s generate at -O3). Parsing is still
  ~1.25 s of standard headers in nearly every unit, 1.8 s for `json.hpp` (281 editor units), 3.6 s
  for `Scene.h`, 7 s for `editor/Project.h` (317 units) and 10 s for `editor/App.h` (M4).
- **Header fan-out is pervasive** (M5): any of the ~70 component headers rebuilds 521 editor units
  (66 % of the editor's compile time) because `EntityRegistry.h` includes every component and
  `Scene.h` includes it; `editor/command/CommandHistory.h` rebuilds 321 units through
  `editor/Project.h`. Touching `Scene.h` cost 775 s in the editor tree, `Vector3.h` 851 s.
- **Single-file edits are dominated by one big unit plus the link** (M2): 73 s for
  InspectorWindow.cpp (the editor), 28 s for MeshSystem.cpp (engine, Release), 60 s in the core
  tests, of which 25 s is the RelWithDebInfo link.
- **Links are slow with debug information** (M6): GNU ld 2.38 (bfd) links the editor tests in
  52.8 s, the editor in 41.1 s, `libjasperengine.so` in 19.9 s and the core tests in 24.9 s
  (RelWithDebInfo); Release links take ~2 s. No lld or mold in the image.
- **ccache** (M7): a new tree at the same path (every worktree is `/src` in the image) hits 100 %
  in direct mode (web: 1,544/1,544, 30 s instead of 447 s). A tree at another path misses
  completely when it has debug information (core tests in another directory: 0 hits, 844 s),
  because ccache hashes the working directory into `-g` compiles. `base_dir` would fix that
  (1,775/1,777 hits, 65 s) but makes the compiler see relative paths: `__FILE__` becomes
  `../src/a.cpp` (tested), which the logger's sources, the profiler's sites and tests that find
  the repository from `__FILE__` (`engine/tests/core/render/ProfilerGpuTest.cpp:79`) depend on.
  No sloppiness is set, so a precompiled header would make every unit uncacheable.
- **Unity builds bit-rotted** (M8): `JASPERENGINE_UNITY_BUILD=ON` fails on oe-port: helper
  collisions (`direction` in SplineEditor.cpp/SplineSceneRender.cpp, `isDocument` in the two VCS
  windows, `shortcutText` in AnimationWindowClipDraw.cpp/PoseAnimationWindow.cpp, an ambiguous
  `color`). Every stream adds file-local helpers, and only the nightly builds it.
- **C++-script games compile the whole engine**: `Exporter::configureBuild/runBuild`
  (`editor/Exporter.cpp:1043-1240`) configure `engine/CMakeLists.txt` as the game's top level, so
  an export compiles ~3,700 CPU-s of engine and vendored code (8 min on 8 cores, without the
  ccache a user rarely has) for a few script files, and again whenever the kit or a project
  setting changes (the kit marker wipes the tree, the settings are global definitions).

## 4. Reference (OpenEngine, MIT; read-only, never executed)

- `CMakePresets.json:679-709`: their editor development preset is ccache with unity builds OFF,
  their clean full build ccache with unity ON. We take the same split (D2), not the file.
- `cmake/AllocationHook.cmake:37`: `SKIP_UNITY_BUILD_INCLUSION` on a source that cannot share a
  unit, the pattern `jasperengine_skip_unity_build` already follows.
- They have no precompiled headers, no linker selection and no prebuilt engine for native game
  code; nothing else is taken.

## 5. Requirements

### Functional — build trees

- **R1. Editor objects compiled once.** The editor's sources (all but `main.cpp`) compile into one
  OBJECT library, `jasperengine-editor-objects`, that `jasperengine-editor`, the Windows
  `jasperengine-editor-cmd` and `jasperengine-editor-tests` link. The command lines, definitions
  and include paths of every editor source stay exactly what `configure_jasperengine_editor_target`
  gives today; only the duplicate compile goes.
- **R2. Precompiled headers.** `engine/cmake/JasperPch.cmake` adds `JASPERENGINE_PCH` (a cache
  BOOL) and `jasperengine_target_pch(<target> [EDITOR])`:
  - the header is `engine/core/JasperPch.h`: the standard headers our units include most (M4: the
    set of 25 costs 1.25 s per unit); `EDITOR` targets use `editor/EditorPch.h`, which adds
    `json.hpp` (1.8 s, 281 units) and yaml-cpp (1.1 s, every unit of `Project.h`). Nothing
    of ours: a precompiled header of `Scene.h`, `Engine.h` or `Project.h` would make every unit
    depend on them (M5) and turn a component edit into a full rebuild;
  - applied to C++ sources of our own targets only: vendored sources compiled inside our targets
    (ImGui, the backends under `libs/`) are skipped, as are C, Objective-C and Objective-C++;
  - on GCC and Clang with Ninja or Makefiles. Not on MSVC (four of our sources define
    `_USE_MATH_DEFINES` before `<cmath>`, which a precompiled `<cmath>` would defeat) or the Xcode
    generator; the option is then ignored with a status message;
  - default ON in the editor tree, OFF in `engine/` (every export, Android and web build and every
    S30 player configure exactly as today; a source with options of its own, such as `-mavx2`,
    compiles without it); `tools/build.sh` turns it on for its Linux development
    and test trees (`linux`'s engine sample, `unit-tests`, `htmlui-tests`, `htmlui-editor-tests`).
- **R3. Linker.** `JASPERENGINE_LINKER` (cache STRING: `AUTO`, `DEFAULT`, `LLD`, `MOLD`, `GOLD`)
  in both trees, applied through `CMAKE_LINKER_TYPE`:
  - `AUTO` picks lld when it is installed, for Debug and RelWithDebInfo builds only, and never for
    a shipped game (S30's `jasperengineShippedGame`): every Release, MinSizeRel and shipped
    artifact keeps the platform's default linker, so the editor release, players and Compile builds
    link exactly as before;
  - an explicit value that the toolchain cannot use (lld missing, mold with GCC < 12.1) is a
    configure error naming the fix;
  - `docker/linux.Dockerfile` installs lld (Ubuntu 22.04's `lld`, LLVM 14). mold is not used: GCC 11
    cannot select it (`-fuse-ld=mold` needs GCC 12.1).
- **R4. Vendored libraries reused across trees.** Each vendored library compiles with only what it
  needs: the trees' directory-wide definitions (editor flags, an export's project settings, the
  player id) move to the targets that read them, so the objects of a library are the same command
  line in the editor, engine, test, player and export trees of one configuration and ccache keeps
  one copy (the content key: sources, headers, flags, compiler). The libraries the measurement
  justifies and that merge cleanly (M9, M10: Jolt) are unity-built (`UNITY_BUILD`) in the trees
  that ship nothing (`JASPERENGINE_VENDORED_UNITY`, OFF for a shipped game, whose code generation
  stays as it was).
- **R5. Project settings reach the engine at run time.** The per-project definitions an export
  injects (window, vsync, title, app id, storage id, icon, log, crash and hang settings, the Lua/C++
  init switches) are compiled only into the game's own executable (`platform/` sources and the
  project), which hands them to the engine through `PlatformConfig` (S30); `jasperengine` and the
  vendored libraries no longer see them. Values and behaviour are unchanged; a settings change
  recompiles the few units that read them instead of the whole engine.
- **R6. Fan-out.** The component headers stop reaching every unit through
  `EntityRegistry.h`/`Scene.h` (M5: any of them rebuilds 521 editor units): `EntityRegistry.h`
  keeps what its own declarations and inline templates need, and each source includes the
  components it names. Only includes move: no declaration, layout, ABI or behaviour changes. M4
  rules out the cheaper cuts: each component header alone parses in 1.2-1.9 s, mostly the shared
  base (`Engine.h`, render types, std), and `editor/Project.h` (7 s) and `editor/App.h` (10 s)
  hold their members by value, so forward declarations alone cannot lighten them.
- **R7. Unity builds work again.** The collisions of M8 are fixed (unique helper names), so
  `UNITY=1 tools/build.sh linux` and `unit-tests` build warning-free under WERROR and pass.
  Whether CI's clean builds use it is decided by AC2's measurement: unity is adopted for a CI job
  only if it saves at least 15 % of that job's clean build over PCH alone.

### Functional — C++-script games

- **R8. Prebuilt engine package.** The engine tree builds a *package* of the engine library for
  one configuration (`cmake -S engine -DJASPERENGINE_ENGINE_PACKAGE=<dir>`, target
  `jasperengine-package`): `lib/` with `libjasperengine.a` and the vendored archives it links,
  `JasperEnginePackage.cmake` with IMPORTED targets carrying their interface definitions, include
  directories and link libraries, and `engine-package.json` with its key (R9).
- **R9. Key.** A package's key is the SHA-256 of: the engine version (`JASPERENGINE_EDITOR_VERSION`
  plus the engine sources' content hash for development builds), the compiler id, version and
  path, the target system and architecture, CMake build type, graphics backend, C++ standard,
  and every option that changes the library or its headers (physics 2D/3D, navigation, runtime
  shaders, development build, HTML UI debugger, Basis transcoder, profiler, release link, split
  symbols, the scene capacities of `SceneMaxValues`, Emscripten thread support and SIMD, Android
  ABI). A game configured with `-DJASPERENGINE_PREBUILT_ENGINE=<dir>` computes the same key and
  fails the configure, listing the differing fields, if the package's differs.
- **R10. Game build against a package.** With `JASPERENGINE_PREBUILT_ENGINE`, `engine/CMakeLists.txt`
  adds no vendored library and no `jasperengine` target: it imports the package and compiles only
  the game executable (platform sources, generated scene code, C++ scripts, `ProjectBuild.cmake`).
  The executable links the same archives as a from-source build, with S30's release link flags,
  symbol splitting and stripping.
- **R11. Exporter.** For a Compile build (S30 R5: C++ scripts, `ProjectBuild.cmake`, larger
  capacities, forced Compile), `Exporter` computes the key, uses
  `<user-data>/jasperengine/engine-packages/<key>/` when it exists, and otherwise builds that
  package once with the same toolchain (written to a staging directory and renamed into place, so
  a cancelled or failed build leaves nothing half-written), then configures the game against it.
  S30's runtime archives may carry the package of their configuration (`tools/build.sh players`
  writes it next to the player); the Exporter uses it when its key matches the user's toolchain,
  which it does for the build images and CI. The Xcode workspace and Android Studio project (source
  exports) are unchanged.

### Performance budgets (server F, 8 jobs; measured, AC1)

- Clean editor build: at least 25 % fewer compile CPU-seconds than the baseline (M1, 10,140).
- Editor-tests tree clean build: at least 35 % fewer compile CPU-seconds (M1, 24,998).
- RelWithDebInfo relink of the core tests after a one-file edit: at most half the baseline link
  (24.9 s).
- Touching a component header in the editor tree rebuilds at most 25 % of the units (today 521 of
  2,214, M5).
- Second export of a C++-script fixture (tools/fixtures/prefab-runtime) to a new directory, without
  ccache: at most 20 % of the baseline time; first export no slower than the baseline plus 10 %.

### Developer & agent UX

- **R12.** `tools/build.sh` passes `CCACHE_SLOPPINESS=pch_defines,time_macros` in the Linux images
  (what ccache needs to cache a unit with a precompiled header; no Linux unit uses `__DATE__` or
  `__TIME__`, the only use is the web-only build id in `core/diag/CrashHandler.cpp`, and the web
  image is not given it). `base_dir` is not set (M7).
- **R13.** `tools/perf/build-bench.sh` and `tools/perf/build_stats.py` stay in the repository and
  `docs/testing.md` gets a "Build speed" section: the options, what is on by default where, how to
  measure, and the ccache settings.

### Data & compatibility

- **R14.** Shipped binaries are unchanged except where intentionally better: release editor,
  players and Compile games keep their linker, flags and optimisation; R5 changes where settings
  are compiled, not their values; R6/R7 move only declarations. AC6 compares the shipped
  executables' sections and symbols before and after.

## 6. Design constraints & decisions

- **D1. Measure first, every lever measured again.** `tools/perf/build-bench.sh` (same phases for
  before and after, same server, load recorded) is the only source of numbers in this spec.
- **D2. Dev loops stay non-unity.** A unity unit recompiles its 16 sources for a one-line edit;
  OpenEngine's development preset is unity OFF for the same reason. PCH helps both loops.
- **D3. ccache is the content-keyed cache for vendored objects.** Its key (preprocessed or direct
  hash of sources, headers, flags and compiler) is exactly the "content key per toolchain and
  config" a prebuilt vendored cache would need, at object granularity, without a second cache
  format to keep correct. R4 makes the command lines identical across trees so it hits.
- **D4. No `base_dir`, no `hash_dir = false`.** The first changes `__FILE__` (M7). The second
  would give reused `-g` objects another tree's working directory in their debug information; the
  gain (another directory of the same configuration) is rare once R4 lands.
- **D5. lld, not mold** (R3): GCC 11 cannot select mold, and lld supports every option we pass
  (`--exclude-libs`, `--version-script`, `--gc-sections`).
- **D6. The package is the engine library, not the player.** S30's player is the engine library
  plus `engine/player`; a Compile game is the same library plus generated scenes and scripts. One
  package format serves both, and the player id definition stays on the player's own sources.
- **D7. Shared ccache across servers: not now** (section 9).

## 7. Acceptance criteria

- **AC1. Before/after table.** `build-bench.sh` on server F, baseline (the oe-port this branch
  merges last) and this branch, same day: clean editor, clean engine, core and editor tests,
  incremental single-file edit, header touch (`Scene.h`, a component header, `Vector3.h`), link,
  web, Android, players, and the C++-script export. Budgets of section 5 met; recorded below.
- **AC2. Unity.** `UNITY=1 WERROR=1 tools/build.sh linux` and `UNITY=1 tools/build.sh unit-tests`
  pass; the clean-build comparison PCH vs PCH+unity is recorded and decides R7.
- **AC3. Warning-free.** `WERROR=1 tools/build.sh linux` (GL and Vulkan), `WERROR=1 tools/build.sh
  unit-tests`, web, web threads and Android build; `JASPERENGINE_PCH=OFF` builds too.
- **AC4. Tests.** Core and editor unit tests, `SANITIZE=address,undefined tools/build.sh
  unit-tests`, one full MCP run (`MCP_SUITE=all`, `JASPER_SLOW_BUILD=1`), `tools/smoke-test.sh`,
  `tools/xcode_sync_*.py --check`, the Android emulator check and S30's player CLI test pass.
- **AC5. Package.** A unit/CTest check configures a game against a package with one changed key
  field and expects the configure error naming it; the prefab-runtime fixture exported twice (new
  directory, ccache off) compiles no engine source the second time (Ninja's log has only the game's
  units) and its screenshot matches the from-source build.
- **AC6. Shipped output.** The release editor install, a release player and a Compile export,
  built before and after: same linker (`readelf -p .comment`), same exported dynamic symbols,
  stripped as before; section sizes within 1 % (R5 moves a few constants).

## 8. Dependencies & conflicts

- Depends on S02 (presets, unity option, WERROR, test targets) and S30 (release link,
  `PlatformConfig`, players, runtime archives, `chooseBuildMethod`). CMake work starts after S30
  lands on oe-port.
- Merge hotspots: `CMakeLists.txt` and `engine/CMakeLists.txt` (S30, S32, S27, SYNC#3): the new
  logic lives in `engine/cmake/JasperPch.cmake`, `JasperLinker.cmake` and
  `JasperEnginePackage.cmake` with one-line calls in the trees. `tools/build.sh` (S30, BF9).
  R6 touches the includes of many sources: done last, mechanically, right before the final merge,
  so the other streams' new sources are covered.
- BF9 owns `tools/mcp/tests`; nothing there changes.

## 9. Risks & defaults

- **A precompiled header hides a missing include.** Default: the engine tree and every export
  build without it (smoke test, web, Android, players), and AC3 builds the editor with
  `JASPERENGINE_PCH=OFF`.
- **Unity builds hide macro leaks between sources.** Vendored libraries are unity-built only
  where M9 shows a gain and the library's sources build in a unit without collisions; our own code
  stays non-unity in dev builds (D2).
- **lld output differs from bfd's.** Only Debug/RelWithDebInfo development and test binaries use
  it; their tests are the check.
- **Package from another toolchain.** The key includes the compiler path and version, so a package
  is never linked into a game built by another compiler.
- **Shared ccache across servers (follow-up, not done).** ccache 4.5 supports secondary storage
  (HTTP, Redis, file). It needs a server reachable from every build host, authentication, and
  trust in what other hosts write into it; a poisoned entry would be linked into every build. The
  hosts are short-lived rentals; each keeps a 40 GB local cache that already gives 100 % hits for
  a new worktree (M7). Revisit with a CI-owned cache.

## Measurements

### M0. Setup

- Server F: Hetzner cpx51, AMD EPYC-Rome, 16 shared vCPU (1 thread per core), 30 GB, no swap,
  SSD. Shared with other streams' jobs: its load average was 4-11 during the baseline and up to
  24 later, so every result records the load before and after, and compile **CPU-seconds** (the
  sum of `.ninja_log` durations, `build_stats.py log`) are given next to wall times as the measure
  less sensitive to other jobs. No server was idle for hours at a time; F is the one used for every
  number here.
- Image `jasperengine/linux-build` (docker/linux.Dockerfile): GCC 11.4.0, GNU ld 2.38 (bfd; gold
  installed, no lld or mold), CMake 4.4.5, Ninja 1.10.1, ccache 4.5.1 (web image: emcc 6.0.10,
  CMake 3.28.3, ccache 4.9.1; Android: NDK clang through Gradle, ccache 4.5.1). The shared ccache
  is `~/je/main/build/.cache/ccache`, `max_size = 40G`, no other setting.
- Every build: 8 jobs in a container capped at 8 CPUs (`JOBS=8`, `docker --cpus 8`), the trees'
  presets as `tools/build.sh` uses them.
- Baseline `base`: oe-port b620283cd, `tools/perf/build-bench.sh base <scenario>`. Phases: cold =
  new directory, `CCACHE_RECACHE` (everything compiles); warm = new directory, ccache on; edits =
  `touch`, rebuilt with `CCACHE_DISABLE=1` (a real edit recompiles).

### M1. Clean builds (baseline `base`, 8 jobs)

| Tree (preset, target) | Cold wall | Compile CPU-s | own / vendored CPU-s | Warm (ccache, same path) | No-op |
|---|---|---|---|---|---|
| editor (`linux-release`, jasperengine-editor) | 1,296 s | 10,140 | 8,317 / 1,810 | 172 s | 1.0 s |
| engine (`engine-linux-release`, jasperengine-project) | 482 s | 3,710 | 2,350 / 1,356 | 53 s | 0.1 s |
| core tests (`engine-linux-tests`, RelWithDebInfo) | 840 s | 6,361 | 4,747 / 1,578 | 844 s (other dir) | 0.2 s |
| editor tests (`linux-ci`, RelWithDebInfo) | 3,271 s (load 10-19) | 24,998 | 22,840 / 2,030 | — | — |
| web (`engine-web-debug`, e3f0c274d) | 447 s | 3,436 | 1,995 / 1,428 | 30 s | 0.1 s |
| Android, 4 ABIs (Gradle debug, e3f0c274d) | 1,596 s | ~6,600 per ABI | ~4,100 / ~2,500 per ABI | 97 s | — |

- The editor tree's targets: jasperengine-editor 5,938, jasperengine 2,411, rmlui 498, Jolt 302,
  shader compiler 266, basisu encoder 164, dav1d 116 CPU-s. The editor-tests tree adds
  jasperengine-editor-tests 9,078 (the editor's sources again) and its test objects 2,113.
- Android's Ninja runs more jobs than the container's 8 CPUs (parallelism 17), so its CPU-seconds
  are inflated; the wall time is the measure there. Players are measured after S30 lands (AC1).
- "Warm" for the editor shows preprocessed-mode hits: its units name the build directory
  (`-I<build>/resources`, the generated headers), so a different directory misses direct mode.

### M2. Incremental builds (warm tree, `CCACHE_DISABLE=1`)

| Tree | Edit one source | Touch `Scene.h` | Touch `Vector3.h` | Other |
|---|---|---|---|---|
| editor | 73 s, InspectorWindow.cpp (3 steps) | 775 s, 523 steps | 851 s, 726 steps | `editor/App.h` 211 s, 93 steps |
| engine | 28 s, MeshSystem.cpp | 139 s, 147 steps | 206 s, 334 steps | |
| core tests | 60 s, MeshSystem.cpp (link 25 s) | 288 s, 194 steps | 448 s, 492 steps | |
| web | 19 s, MeshSystem.cpp | | | |
| Android, 4 ABIs | 93 s, MeshSystem.cpp | | | |

### M3. Slowest units (`build_stats.py time-report`, GCC `-ftime-report`, Release)

| Unit | Logged | Parse + deferred | Opt + generate | (templates) |
|---|---|---|---|---|
| editor/mcp/tools/WorldTools.cpp | 79.6 s | 24.9 | 59.4 | 15.2 |
| editor/window/InspectorWindow.cpp | 74.0 s | 22.6 | 55.7 | 13.8 |
| engine/core/registry/EntityRegistry.cpp | 62.6 s | 13.4 | 55.1 | 7.9 |
| engine/core/script/binding/GameObjectClassesLua.cpp | 58.9 s | 20.2 | 47.3 | 12.5 |
| editor/mcp/tools/AssetTools.cpp | 51.0 s | 21.9 | 33.4 | 13.1 |
| editor/Project.cpp | 47.2 s | 21.2 | 31.5 | 12.9 |
| engine/libs/harfbuzz/harfbuzz.cc | 36.6 s | 11.5 | 29.4 | 6.5 |
| engine/tests/core/ecs/SignatureTest.cpp (RelWithDebInfo) | 182 s | | | |

### M4. Header parse costs (`build_stats.py parse`, with InspectorWindow.cpp's flags)

- 25 standard headers together (`<string> <vector> <functional> <unordered_map> <map>
  <algorithm> <memory> <filesystem> <mutex> <chrono> <sstream> <optional> <variant> <thread>
  <atomic> <array> <set> <unordered_set> <cmath> <cstring> <cstdint> <string_view> <utility>
  <limits> <fstream>`): 1.25 s (the most expensive alone: `<filesystem>` 0.72, `<thread>` 0.63,
  `<algorithm>` 0.51, `<memory>` 0.51). They reach 1,100-1,640 of the editor tree's 2,214 units.
- `json.hpp` 1.78 s (281 editor units, 20 engine units); `imgui.h` 0.04 s; `sokol_gfx.h` 0.02 s.
- Our hubs: `editor/App.h` 9.99 s (91 units), `editor/Project.h` 7.04 s (317), `Scene.h` 3.56 s
  (517), `GameObject.h` 3.70 s (394), `RenderSystem.h` 2.76 s (355), `Engine.h` 1.06 s (600); any
  component header 1.2-1.9 s alone (RigidbodyComponent.h 1.92 with Jolt's `Body.h`).

### M5. Header fan-out (`build_stats.py fanout`, editor tree, 2,214 units, 10,127 CPU-s)

| Header | Units it rebuilds | Their compile CPU-s |
|---|---|---|
| engine/core/Export.h | 1,033 | 8,193 (81 %) |
| engine/core/math/Vector3.h | 720 | 7,340 (72 %) |
| engine/core/Log.h | 673 | 7,221 (71 %) |
| engine/core/Engine.h | 600 | 7,015 (69 %) |
| any component header (MeshComponent.h, LightComponent.h, ...) | 521-529 | ~6,700 (66 %) |
| engine/core/registry/EntityRegistry.h | 521 | 6,694 (66 %) |
| engine/core/Scene.h | 517 | 6,615 (65 %) |
| engine/core/object/GameObject.h | 394 | 5,568 (55 %) |
| engine/core/subsystem/RenderSystem.h | 355 | 5,467 (54 %) |
| editor/command/CommandHistory.h | 321 | 5,013 (50 %) |
| editor/Project.h | 317 | 4,991 (49 %) |
| editor/App.h | 91 | 1,737 (17 %) |

The top 25 own headers all rebuild 600-1,033 units: everything below `Engine.h` and the math
types is one cluster. In the engine tree a component header rebuilds 148 of 582 units.

### M6. Links (GNU ld 2.38)

| Output | Release | RelWithDebInfo |
|---|---|---|
| libjasperengine.so | 2.2 s | 19.9 s |
| jasperengine-editor | 2.1 s | 41.1 s |
| jasperengine-editor-tests | — | 52.8 s |
| jasperengine-core-tests | — | 24.9 s |
| jasperengine-project (engine sample) | 2.0 s | — |
| web jasperengine-project.html (wasm-ld) | — | 7.3 s (Debug) |

### M7. ccache

- Same path, new tree (what a new worktree is): web 1,544 of 1,544 direct hits (30 s for a 447 s
  build); engine 53 s for 482 s; Android 6,179 direct hits (97 s for 1,596 s).
- Another path with `-g`: core tests in `core-tests-other`, 0 hits (844 s). With
  `CCACHE_BASEDIR=/src CCACHE_NOHASHDIR=1`: 1,775 hits of 1,777 (65 s), but `base_dir` hands the
  compiler relative paths (`__FILE__` printed `../src/a.cpp` in a test compile), so it is not used.

### M8. Unity build of our code (S02's `JASPERENGINE_UNITY_BUILD`, e3f0c274d)

The editor tree has 1,555 steps instead of 2,255, but fails: `direction` (SplineEditor.cpp,
SplineSceneRender.cpp), `isDocument` (VcsCommitWindow.cpp, VcsConflictsWindow.cpp),
`shortcutText` (AnimationWindowClipDraw.cpp, PoseAnimationWindow.cpp) and an ambiguous `color`
in PoseAnimationWindow.cpp, among others.

### M9. Unity builds of the vendored libraries (engine tree, Release, ccache off, e3f0c274d)

`CMAKE_UNITY_BUILD=ON` (16 sources a unit) on the vendored targets only, `ninja -k 0`:

| Library | Plain CPU-s | Unity CPU-s | Unity result |
|---|---|---|---|
| joltphysics | 248 | 68 | builds |
| rmlui | 394 | 101 (3 units failed) | `IsEscapedCharacter`, `RunMode` collide |
| shadercompiler (glslang, SPIRV, spirv-cross) | 151 | 13 (2 failed) | glslang's generated parser does not merge |
| dav1d | 140 | 112 | builds (C, per-bitdepth templates) |
| thorvg | 58 | 17 (1 failed) | `_find` collides |
| soloud, recastnavigation, opus, stb | 25, 14, ~10, 12 | (1 failed each) | `catmullrom`, `prev`, static/extern clash, ... |
| all 25 targets | 1,236 (166 s wall) | 478 of the units that built (71 s) | 10 failed units |

The C libraries gain little (dav1d −20 %) for the most macro-leak risk; Jolt (−73 %) and RmlUi
(~−70 % once its colliding sources are kept apart) carry most of the gain.

The editor tree's own unity build (`ninja -k 0`) fails in 20 units: anonymous-namespace
`Registry`, `fail`, `isInside`, `lower`, `isDescendantOf`, `useProject` and differently-aliased
`Json` types in the animation documents, the lattice bake, the layout presets, the organisation
labels and the pose window, besides those of M8.

### M10. First lever measurements (`lev1`, e3f0c274d plus the uncommitted R1-R3 wiring, server F)

| Tree | Baseline (M1/M2) | With PCH, editor objects once, lld (Debug/RelWithDebInfo) |
|---|---|---|
| editor clean | 1,296 s, 10,140 CPU-s | 1,115 s, 8,686 CPU-s (own code 8,317 -> 6,930: jasperengine 2,411 -> 1,781, editor 5,938 -> 5,169) |
| editor warm, same path | — | 38 s (2,167 direct hits of 2,171: PCH units cache with the sloppiness) |
| editor, edit InspectorWindow.cpp | 73 s | 66 s |
| editor, touch Scene.h | 775 s, 523 steps | 653 s, 525 steps |
| core tests clean | 840 s, 6,361 CPU-s, link 24.9 s | 728 s, 5,364 CPU-s, link 2.1 s |
| core tests warm, same path | — | 58 s |
| core tests, edit MeshSystem.cpp | 60 s | 36 s |
| core tests, touch Scene.h | 288 s | 226 s |
| editor tests clean | 3,271 s, 24,998 CPU-s, links 114.8 s | 1,609 s, 12,621 CPU-s, links 9.2 s (editor tests 52.8 -> 3.5 s, editor 41.1 -> 2.7, libjasperengine.so 19.9 -> 2.7) |

Jolt unity (R4): 248 -> 68 CPU-s in the engine tree, and `nm --defined-only -g` of
libjoltphysics.a lists the same 1,739 strong symbols either way (only weak template instances
differ, as inlining does). RmlUi unity was dropped: besides M9's collisions, StyleSheetSelector.cpp,
TemplateCache.cpp and StyleSheetSpecification.cpp each define a file-scope `instance` or a helper
another file has, and ~15 more `static` helper names repeat across its sources; a skip list that
long would have to be re-derived on every RmlUi update. PCH parse costs: `JasperPch.h` 1.22 s,
`EditorPch.h` (with json.hpp and yaml-cpp) 1.88 s + 1.12 s for yaml-cpp.

## Progress

- 2026-10-30: measurement tools committed (`tools/perf/build-bench.sh`, `tools/perf/build_stats.py`);
  baseline run `base` on server F (b620283cd).
- 2026-10-31: oe-port e3f0c274d merged (S29 fix, S32); web, Android, unity, ccache and header
  measurements on it; spec written from M1-M8. Next: M9 (vendored unity), then wait for S30 before
  any CMake change.
