# S34 — Player size

Wave 4. Depends on S30 (prebuilt players, the release link, `lib_size.py`, `apk_size.py`,
`android_size.sh`), S32 (the shader warm-up list), S23 (the Lua API description,
`tools/lua_api`), S08 (the reflection tables) and S33 (the linker selection).

## 1. Goal

A shipped game carries only the engine code it can use. The export already knows everything a
project uses (its scenes, prefabs, scripts, documents and content), so the player it ships leaves
out the modules nothing in the project can reach: the HTML UI, video, the runtime shader compiler,
3D or 2D physics. The engine code that every game keeps gets smaller too: the Lua bindings' template
thunks, the Linux release link (LTO), and size-optimised code where it costs no speed. A game
behaves exactly as it did; only its download shrinks. The bar is Unity's player sizes or better.

## 2. Scope

**In scope**
- Fresh measurements of the release players of every target the build servers make (Android, all
  four ABIs; Linux glcore, gles3, vulkan; web single-thread and threaded), by module (section M).
- Per-project module stripping for both build methods: prebuilt player variants (no compiler), and
  the same module switches in a Compile build.
- The detection of what a project uses, correct by construction, with a test per module.
- No runtime shader compiler in a game whose scripts cannot ask for a variant the cook did not
  build.
- Smaller Lua bindings (shared thunk code, no per-thunk error paths).
- LTO for the Linux release players.
- `-Oz`/`-Os` evaluated for the size-critical targets against the benchmarks on server E, adopted
  only where the cost is negligible.
- Android: the C++ runtime linked statically into the one engine library.

**Out of scope**
- A linker on the user's machine (option b of the brief; see D1).
- Dynamic modules (an engine split into shared libraries loaded on demand): no LTO across them, a
  second copy of the C++ runtime and of every shared template, and no WebAssembly equivalent.
- Content size (textures, audio, meshes): S14 and the cook own it.
- Windows, macOS and iOS measurements: no build server makes them. Their players get the same
  variants and flags through the same CMake code; CI measures them.
- Changing what any feature does.

## 3. Current state

Measured in section M (oe-port f92d8a732). In short:

- Every prebuilt player has every module (`package_player.py` `FEATURES`, `player-info.json`
  `features`). A Compile build switches physics, navigation and the runtime shader compiler from
  Project Settings only (`Exporter.cpp`, `JASPERENGINE_PHYSICS_2D/3D`, `JASPERENGINE_NAVIGATION`,
  `JASPERENGINE_RUNTIME_SHADERS`); the HTML UI (RmlUi, lexbor, ThorVG, msdfgen, the Lucide icons,
  the compatibility layer, widgets, effects, anime) and video (dav1d, Opus, libwebm) are always in.
- The release link (S30): sections and `--gc-sections`, hidden visibility; ThinLTO and `--icf=safe`
  on Android only. Linux players link with GNU ld and no LTO; the web with wasm-ld, no LTO.
- Android ships `libc++_shared.so` next to the one engine library.
- The Lua bindings are LuaBridge 3.01 (`engine/libs/luabridge3`): every bound function and
  property instantiates its own thunk with its own argument checks, error messages and exception
  handler.
- The runtime shader compiler (glslang, SPIRV-Cross and the embedded shader library) builds
  variants a build lacks. The export's collector (`ShaderVariantCollector`, `collectRequiredShaderKeys`)
  already cooks what the content, the prefabs a script can spawn, and the scripts' light-capacity,
  shadow-quality, lit-quality and post-processing switches reach, plus S32's warm-up list; only a
  script that builds meshes, materials or lights the editor never saw needs the compiler.

## 4. Reference

OpenEngine has no per-project stripping or size work to take (checked: its CMake and web tools).
The model is Unity's: "Strip Engine Code" (Player Settings > Optimization) leaves out the engine
modules a build does not use, from what the build references, and the managed code stripping
level. We do the same at module granularity, decided by the editor from the project's content.

## 5. Requirements

### Functional — modules

- **R1. Modules.** The engine's optional modules, each a CMake option that defaults ON (the editor,
  the tests and the full player have all of them) and a definition the code is compiled with:

  | module | what it is | libraries | option |
  |---|---|---|---|
  | `htmlui` | HTML/CSS UI documents, their Lua API, anime, widgets, effects, virtual controls | rmlui, lexbor, thorvg, msdfgen, lucide | `JASPERENGINE_HTMLUI` |
  | `video` | the HTML UI's `<video>` | dav1d, opus, libwebm | `JASPERENGINE_VIDEO` (needs `htmlui`) |
  | `runtimeshaders` | the runtime shader compiler | glslang, spirv-cross, the shader library | `JASPERENGINE_RUNTIME_SHADERS` |
  | `physics3d` | Jolt bodies, joints, character controllers, buoyancy | joltphysics | `JASPERENGINE_PHYSICS_3D` |
  | `physics2d` | Box2D bodies, joints, character controllers | box2d | `JASPERENGINE_PHYSICS_2D` |
  | `navigation` | navmeshes and crowds (grids stay) | recastnavigation | `JASPERENGINE_NAVIGATION` |

  A module that is off compiles to nothing: its sources, libraries, Lua bindings and notices are
  left out. The engine code that talks to it either leaves the calls out (`#if`) or, for the HTML
  UI's document system that the input routing and rendering call everywhere, gets the document
  system of a build without documents (D3).
- **R2. Identical behaviour.** A game that uses no part of a module behaves the same with and
  without it: the same frames, logs, transcripts and exit codes. A module is only left out when the
  detection (R5) proves nothing can reach it.
- **R3. Players know their modules.** A player reports the modules it was compiled with
  (`player::builtModules()`, from the same definitions), `player-info.json` `features` lists them,
  and `content-manifest.json` lists the modules the content needs (`modules`). A player whose
  modules do not cover the content's exits with code 5 and names the missing ones. The payload
  validator refuses such a build before it ships.

### Functional — variants and selection

- **R4. Prebuilt variants.** `engine/player/variants.json` names a few module sets. A release
  player runtime is built for each (`-DJASPERENGINE_PLAYER_VARIANT=<name>`): its id is
  `<platform>-<arch>-<backend>-release[-<variant>]`, the full player keeping today's id. The
  development flavour is always full. The set is chosen from the measurements (D2).
- **R5. Detection.** The export computes the modules a project needs (`editor/players/ModuleUsage`)
  from:
  - the components of every scene and prefab of the build, and the component fields that make the
    engine create a module's objects (a Spline Extrude's collision);
  - the project's Lua scripts: the Lua names of each module, read from the Lua API description
    (the bindings' `#if` conditions, so a binding added under a module's condition is detected
    without a list to update), every name whose type is one of the module's classes, and the
    dynamic accesses that could reach any name (`_G`, `_ENV`, `load`, `rawget`, ...), which need
    every module;
  - the script properties of scenes and prefabs (their object types);
  - the HTML UI documents (video: a `<video>` element), and C++ scripts (every module the project
    settings allow);
  - for the runtime shader compiler, whether any script can reach a shader input the cook did not
    see (D5).

  Each needed module comes with its evidence (`scene Main: Rigidbody on Player`,
  `lua/hud.lua:12: UIDocument`), shown in Build Settings, the CLI and MCP.
- **R6. Selection.** A prebuilt build uses the smallest installed release variant whose modules
  cover the needed ones (the full player always does). When a smaller variant fits but is not
  installed, the build uses the best installed one and warns `W-PLAYER-VARIANT` with the id to
  download; Build Settings offers its download. A Compile build switches each module off that the
  project does not need (R5) or does not allow (Project Settings), unless it has C++ scripts or
  `ProjectBuild.cmake` (their code can use anything: the settings alone decide, as today).
- **R7. Strip Engine Code.** Project Settings > Player > Strip Engine Code (`player.strip_engine_code`,
  `project.yaml` `stripEngineCode`, default on) turns R5/R6 off: every module the settings allow.

### Functional — runtime shaders

- **R8.** With Strip Engine Code on, a build leaves the runtime shader compiler out when no script
  can ask for a variant the cook did not build (D5); Runtime Shader Compilation off still leaves it
  out of a Compile build regardless. A prebuilt build that keeps the compiler keeps today's
  behaviour exactly.

### Functional — code size

- **R9. Lua bindings.** The bindings' thunks share their error handling and argument-failure paths
  (one out-of-line function each instead of a copy per thunk); the messages and behaviour are the
  same (the Lua API doc test, `LuaBinding*` tests and the player CLI test hold).
- **R10. Linux LTO.** Linux release players and Compile builds link with GCC LTO (`-flto=auto`) when
  the measured link is reliable (D7).
- **R11. Size-optimised code.** `-Os`/`-Oz` is applied where the benchmarks on E show a negligible
  cost (≤ 1 % on the hot-path benches, D8), and documented per target.
- **R12. Android C++ runtime.** The Android player links libc++ statically into
  `libjasperengine-android.so` (no `libc++_shared.so`), so the release link drops what nothing uses.

### Platform & backends

- **R13.** Every module switch builds on every target and backend (GL 3.3, GLES3/WebGL2, Vulkan,
  Metal, D3D11; Windows, Linux, macOS, Android, iOS, web). The build servers build Linux, web and
  Android variants; CI builds every runtime of every variant.

### Performance

- **R14.** No player variant or flag in this spec costs frame time: the benches on E (D8) and S30's
  headless budget (600 frames ≤ 15 s) hold.
- **R15. Size targets** (measured in M; the fixture `tools/fixtures/unified-lighting`, which uses no
  HTML UI): the Android arm64 Play download of a project without HTML UI and with no
  runtime-generated shaders ≥ 30 % smaller than S30's 12.4 MiB; every target's player smaller.

### Editor UX

- **R16.** A finished build in Build Settings shows the player runtime it used (its variant), the
  modules it ships and those it left out, and why each kept module is needed (the evidence in a
  tooltip); the Console logs the same line. Strip Engine Code is a Project Settings row, applied
  with the other settings.

### Agent UX

- **R17.** `jasperengine-editor build` prints `player:` (as today), `modules:`, `left_out:` and a
  `module:` line with the first reason of each; `build --json` adds `modules`,
  `modules_left_out` and `module_evidence`. MCP `build_project` reports `modules` and
  `module_reasons` in its structured result (no schema change: the tool budget is unchanged). The
  shipping guide page explains variants and Strip Engine Code.

### Data & compatibility

- **R18.** New keys only: `player-info.json` `variant` (absent: full), `content-manifest.json`
  `modules` (absent: every module, as an older build's content needs), `project.yaml`
  `stripEngineCode` (absent: on). Old players read new content the same way (unknown keys are
  ignored); a new player reading old content without `modules` needs every module and so refuses
  only to run in a variant, which no old build selected. No scene format change.

## 6. Design constraints & decisions

- **D1. Prebuilt variants, not a link on the user's machine.** S30's promise is no compiler, no
  linker, no NDK or emsdk. Linking prebuilt static archives per project (option b) would ship a
  linker for every host and target (lld is tens of MB per host) and LTO bitcode archives (hundreds
  of MB) with the editor, and would break the "no toolchain" promise in spirit. Feature flags in a
  compiled build (option c) need a compiler. So prebuilt builds choose among a few variants built
  by CI (option a), and Compile builds use the same switches as flags (option c), from one
  detection. Correctness by construction:
  - selection only ever picks a variant whose modules ⊇ the needed ones (the full player always
    qualifies);
  - the content records what it needs (`content-manifest.json` `modules`), and the player refuses
    to run what it cannot (exit 5): a wrong selection cannot run silently;
  - the detection reads the Lua API from the bindings' own conditions, and the components through
    the engine's types.
- **D2. The variant set** is the smallest that captures most of the size (section M): see the
  table in M5. Adding one is a line in `variants.json`; CI and `tools/build.sh players` read it.
- **D3. Gating the HTML UI.** The UI Document component stays (its data is cooked like any other).
  The engine's input routing, its rendering and its Lua start-up call the scene's
  `UIDocumentSystem` in about forty places; a build without the HTML UI registers
  `UIDocumentSystemNone.h`'s system in its place, which answers as a document system without
  documents does (nothing consumed, nothing drawn, everything loaded). The routing code and the
  order of the scene's systems stay exactly as they are, so a game without documents runs the same
  instructions it always did around them; the alternative (forty `#if` blocks through the input
  code) risks the very behaviour this spec must keep. The Lua bindings of the HTML UI, the virtual
  controls and anime are under `#if JASPERENGINE_HTMLUI` (the media API under
  `JASPERENGINE_VIDEO`), which is what the detection reads. HtmlUIClock and the video format rules
  (`VideoFormats`, which the compatibility layer applies to every document) stay.
- **D4. The Lua scan.** A Lua lexer (comments skipped, identifiers and the words of every string
  literal kept, `require` of a project module followed). Names come from the Lua API description
  the editor embeds (`LuaApiData`); the parser records each binding's module (`modules`) from its
  preprocessor conditions. A module's entry points are its classes, enums and globals, its members
  of classes outside it, and every member of any class whose type names one of its classes; the
  members of the module's own classes are reachable only through those. The dynamic accesses
  (`_G`, `_ENV`, `getfenv`, `setfenv`, `load`, `loadstring`, `loadfile`, `dofile`, `rawget`,
  `rawset`, `debug`, `package`, and `require` with a computed name) need every module.
- **D5. Runtime shaders.** A shader variant the cook lacks can only be asked for by a script (D4's
  evidence): what the scenes, the prefabs a script can spawn, and the scripts' quality switches
  reach is cooked already (S10, S11, S13, S32 collectors). The Lua API is split into shader-neutral
  and shader-relevant names: `editor/players/ShaderNeutralApi.cpp` lists the classes and members
  that cannot change what an object draws with (math, input, time, audio, physics, transforms,
  activation, scene loading, prefab spawning...), each reviewed; everything else is relevant, so a
  new binding is relevant until reviewed. A unit test holds the neutral classes' member lists
  against the API description (a new member fails until reviewed). The compiler stays when any
  script names a relevant entry point (as in D4) or makes a dynamic access, when there are C++
  scripts, or when Strip Engine Code is off.
- **D6. Lua thunks.** LuaBridge's thunks keep their structure; their failure paths move out of
  line (`[[noreturn]]` helpers taking the error code or the exception message), so every
  instantiation drops its string building and its unwind cleanups. The messages are byte-identical.
- **D7. Linux LTO** with GCC's `-flto=auto`, `gcc-ar`/`gcc-ranlib`, for release games only (as the
  rest of the release link), measured for link time and memory on the build servers.
- **D8. `-Os`/`-Oz`.** Measured with Clang (the compiler of Android and the web) on E's quiet
  cores: the core benches (`je-bench-anim`, `-particles`, `-picking`, `-nav`, `-splines`, `-jobs`,
  `-memory`, the `*Bench*` suites) and a Lua call bench, at the targets' current level and at
  `-Os`/`-Oz`. Adopted per target and per code area only where every hot bench stays within 1 %.
- **D9. Android libc++ static**: `ANDROID_STL=c++_static` for the player (and the Gradle template a
  Compile build exports), measured.

## 7. Acceptance criteria

- **AC1. Measurements** — section M: before/after per target and per module, the Play download per
  ABI (`bundletool get-size total --dimensions=ABI`), from `tools/players/player_sizes.py`,
  `lib_size.py --modules` and `tools/ci/android_size.sh`.
- **AC2. Detection per module** — `ModuleUsage.*` editor tests: for every module, each evidence
  kind on its own makes it needed (a component, a component field, a Lua class name, a member on
  another class, a member typed with the module's class, a script property type, a document
  element, a dynamic access, C++ scripts), and an empty project needs none. The table of a
  module's Lua names is checked non-empty and to contain known entry points. `ShaderNeutralApi.*`:
  every listed name exists and the neutral classes' members are the reviewed ones.
- **AC3. Variants build and run** — the Linux, web and Android variant runtimes build with warnings
  as errors; the player CLI test (`tools/ci/test_player_cli.py`) runs the fixture on the variant the
  build selects and matches the full player's golden; `PlayerModules.*` core tests.
- **AC4. Refusal** — a content manifest needing a module the player lacks exits 5 naming it
  (`RunControlFixture` or `ContentManifest.*`); the payload validator refuses such a build
  (`PayloadValidator.*`).
- **AC5. Unchanged behaviour** — S30's player CLI test, its goldens, `tools/smoke-test.sh`, the
  Android emulator check with `--relaunch`, the web gate, BF10's upgrade check (prebuilt path
  included), the unit tests, one full MCP run and the `xcode_sync` checks all pass.
- **AC6. Performance** — D8's bench table (M6) and the decision per target.

## 8. Dependencies & conflicts

- Merge hotspots: `engine/CMakeLists.txt`, `engine/core/Engine.cpp`, `editor/ExporterPrebuilt.cpp`,
  `editor/Exporter.cpp`, `tools/build.sh`, `tools/players/*`, `.github/workflows/ci.yml`,
  `engine/workspaces/androidstudio/app/build.gradle`, the Lua bindings.
- The Xcode workspace lists engine sources: a module switch there follows the existing
  `EXCLUDED_SOURCE_FILE_NAMES` pattern (the `xcode_sync` checks).

## 9. Risks & defaults

- **A script that builds a Lua name at runtime from pieces** (`obj["get" .. "Rigidbody"]`, a name
  read from a file) is the one use the scan cannot see. A global needs `_G`, `_ENV`, `load` or
  the like, which the scan treats as using everything; a member needs an object, which the script
  gets through a name the scan sees, except a member of a class outside the module (a GameObject's
  `getRigidbody`). Default: that case is documented, and Strip Engine Code turns the stripping off.
- **False positives** (a local named `debug`, a word of a string equal to an entry point) keep a
  module that could have gone; never the reverse. The evidence names the file and line.
- **Variant count against CI time.** Default: release only, and the set in M5; development stays
  full.
- **GCC LTO link memory.** Default: `-flto=auto` (jobserver), measured; off if a build server fails.

## M. Measurements

oe-port f92d8a732 (this branch's base), release players as `tools/build.sh players` builds them,
measured with `tools/players/player_sizes.py` (shipped bytes, gzip -9 for the web, deflate -9 for
Android as an APK stores libraries below minSdk 23), `tools/players/lib_size.py --modules` (named
code and data by S34 module, from the runtime's symbols or the wasm's name section) and
`tools/ci/android_size.sh` (APKs, bundle, `bundletool get-size total --dimensions=ABI`). MiB for
files, MB (10^6) for the module breakdown, which counts named symbols only (alignment, unwind
tables and unnamed data come on top). Servers B (Linux), F (Android), C (web).

**M1. Linux** (GCC 11, GNU ld, sections + `--gc-sections`, no LTO): the stripped player is
41.75 MiB for glcore and gles3, 41.76 for vulkan. Of it: `.text` 31.6, `.rodata` 3.7, `.eh_frame`
2.8, `.rela.dyn` 1.2 (PIE), `.gcc_except_table` 0.75, `.eh_frame_hdr` 0.45.

**M2. Android** (NDK Clang, `RelWithDebInfo` = `-O2`, ThinLTO, `--icf=safe`, gles3):

| ABI | libjasperengine-android.so (deflated) | libc++_shared.so | APK per ABI | Play download |
|---|---|---|---|---|
| arm64-v8a | 28.28 (11.64) | 1.20 | 12.97 | 12.64 |
| armeabi-v7a | 19.80 (10.57) | 0.81 | 11.85 | 11.50 |
| x86 | 34.73 (13.47) | 1.09 | 14.86 | 14.52 |
| x86_64 | 34.73 (13.38) | 1.17 | 14.75 | 14.41 |

The universal APK is 51.61 MiB, the bundle 51.52 MiB. (S30's final gate had arm64 12.40 MiB of
download; S21, S32, S27 and the upstream merges since added about 0.25 MiB.)

**M3. Web** (Emscripten, `-O3`, wasm-ld, `--profiling-funcs`): `jasperengine-player-st.wasm`
28.29 MiB (gzip 8.20), `-mt.wasm` 28.21 (8.15), their `.js` 0.17 and 0.20 (0.04, 0.05). The
largest single function is `JasperEngineWeb::init` at 0.84 MB: Binaryen inlines every function
called once, and the Lua binding registrations are.

**M4. By module** (MB of named symbols):

| module | Linux glcore | Android arm64 | web st |
|---|---|---|---|
| htmlui (rmlui, lexbor, thorvg, msdfgen, lucide, engine) | 8.01 | 4.81 | 4.35 |
| video (dav1d, opus, libwebm, engine) | 0.88 | 0.67 | 0.37 |
| runtimeshaders (glslang, spirv-cross) | 1.33 | 1.44 | 1.02 |
| physics3d (Jolt) | 1.36 | 0.73 | 0.87 |
| physics2d (Box2D) | 0.45 | 0.35 | 0.13 |
| navigation (Recast/Detour, engine navmesh) | 0.51 | 0.35 | 0.38 |
| Lua bindings (luabridge3 templates) | 2.86 | 1.80 | 1.12 |
| C++ standard library | 3.92 | 2.06 | 1.83 |
| Lua | 0.16 | 0.15 | 0.15 |
| core (always) | 18.55 | 13.61 | 13.91 |
| total | 38.04 | 25.96 | 24.13 |

Of the Lua bindings on Linux (no ICF): `Namespace::Class` registration instantiations 0.69 MB
(1,069), `invoke_member_function` and its const twin 0.78 MB (1,728), `invoke_proxy_functor`
(lambdas) 0.38 MB, `tostring_metamethod` 0.26 MB (249 copies of one function that never uses its
class parameter), property getters and setters 0.17 MB, the functors' `__gc` deleters 0.04 MB.

**M5. The variant set** (D2), from M4: the HTML UI is a fifth of every player and the largest
module by far; video is an HTML UI feature many UI games do not use; the runtime shader compiler
and 3D physics are next; 2D physics and navigation are small. Release variants:

| variant | modules | leaves out (Linux / arm64 / web, MB) |
|---|---|---|
| full | all | — |
| ui | htmlui, runtimeshaders, physics3d, physics2d, navigation | 0.9 / 0.7 / 0.4 |
| core | runtimeshaders, physics3d, physics2d, navigation | 8.9 / 5.5 / 4.7 |
| lite3d | physics3d, navigation | 10.7 / 7.3 / 6.0 |
| lite2d | physics2d, navigation | 11.6 / 7.7 / 6.7 |

Navigation stays in every variant (its Project Settings switch keeps deciding the Compile build).
