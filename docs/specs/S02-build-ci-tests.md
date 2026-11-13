# S02 — Build, CI & tests

Wave 0/1. Wave 0 is presets, build options and the unit-test framework. Wave 1 is CI that runs every suite, plus the web browser gate.

## 1. Goal

Every other spec says "testing is part of done", so there has to be one place to put the tests and one thing that runs them. This spec delivers that:
- a googletest framework for engine core and editor logic, seeded with tests for the areas that have none today: ECS, scene serialization, math, undo, asset database and the CPU side of the renderer;
- CI that actually runs the unit, HTML UI, MCP, smoke and sanitizer suites, plus a headless-Chrome gate for web builds;
- standard build entry points: CMake presets, sanitizer builds, a unity-build option, and warnings-as-errors on our own code.

Builds and tests stay reproducible in the Docker images through `tools/build.sh`, so a remote build server and a GitHub runner fail the same way.

## 2. Scope

**In scope**
- `CMakePresets.json` for the editor tree and for the standalone engine tree.
- Sanitizer builds through `JASPERENGINE_SANITIZE`: ASan, UBSan and TSan, with suppression files.
- `JASPERENGINE_UNITY_BUILD`.
- `JASPERENGINE_WARNINGS` and `JASPERENGINE_WERROR`, with a plan for cleaning up the existing warnings.
- Vendored googletest.
- `jasperengine-core-tests` and `jasperengine-editor-tests` with a shared test-support library, and a first batch of tests.
- New `tools/build.sh` targets, a gtest XML gate script, and the CI suite selection for the MCP tests.
- Changes to `.github/workflows/ci.yml`, and a new `.github/workflows/nightly.yml`.
- `tools/web/serve.py` and `tools/web/browser_gate.py`, adapted from OpenEngine.
- A tag/version check in the release job.
- Staging the third-party notices into the editor install.
- `docs/testing.md`.

**Out of scope**
- vcpkg, their 38 KB vcpkg-bound presets, and their DebugFast config (README do-not-copy list; report 10 §6).
- UIReplay.
- Fuzzing.
- A VERSION file (decided against, see D9).
- Staging notices into *exported games*. S30 owns the export/publish pipeline and reuses `tools/ci/check-notices.py` from this spec.
- A gate for exported web projects. S30 adds it with the same `browser_gate.py`.
- Running unit tests under Emscripten, Android or iOS.

## 3. Current state

- **No presets, sanitizers, unity build, PCH or warning flags.**
  - Our own code compiles with compiler defaults. Only vendored libraries get flags, and those are `-w`/`/w` (`engine/CMakeLists.txt:767-778`, `CMakeLists.txt:126-128,493-496`).
  - `grep` finds no `Werror`, `/WX`, `fsanitize`, `UNITY_BUILD` or `CMakePresets` anywhere.
  - The C++ standard defaults to 17 (`engine/CMakeLists.txt:3-7`). S01 moves it to 20.
- **`engine/CMakeLists.txt` doubles as the top-level CMakeLists of every exported project** (`editor/Exporter.cpp:2241-2265`). Any option added there must therefore default to the behaviour a user's game wants.
  - `CopyEditorEngine.cmake:19-24` copies `engine/` into the editor install. It excludes only `engine/tests`.
- **`tools/build.sh`** (210 lines) has the targets `linux|web|android|htmlui-tests|htmlui-editor-tests|shell`.
  - It builds each image by Dockerfile and runs it by image ID (`:71`).
  - It shares one ccache across worktrees (`:65,90,110`) and runs the symbol-leak gate (`:140-160`).
  - The tests run under `xvfb-run` (`:185,197`).
- **Tests that exist today:**
  - `engine/tests/htmlui/` (`jasperengine-htmlui-tests`): about 3,300 tests, 74 of them skipped. A glcore run takes 1,244 s and a Vulkan run 1,078 s (`docs/html-ui/phase-6-acceptance.md` §4.2).
    - Its `harness/` (`TestSystem`, `GpuEnvironment`, `HeadlessContext`, `Readback`, `ImageCompare`) is a generic headless engine harness, not an HTML UI one.
    - `JASPER_REQUIRE_GPU()` fails the test rather than skipping it (`harness/GpuEnvironment.h:50`).
  - `editor/htmlui/tests/` (`jasperengine-htmlui-editor-tests`, 73 tests, Linux only).
    - It links the editor sources without `main.cpp` and links gtest by file so that the editor objects stay ccache-identical (`HtmlUiEditorTests.cmake:24-56`).
  - Both targets borrow googletest 1.11 from `libs/yaml-cpp/test/gtest-1.11.0` (`engine/tests/htmlui/CMakeLists.txt:9-15`), and neither registers anything with CTest.
  - `EditorLogicTests.cpp` sits in the HTML UI suite only "because the editor has no googletest target" (`engine/tests/htmlui/CMakeLists.txt:28-34`).
- **Untested areas.** None of the following has a unit test:
  - the ECS: `engine/core/ecs/*` and `engine/core/registry/EntityRegistry.*`, with `Signature = std::bitset<64>` (`engine/core/ecs/Signature.h:12`);
  - math: `engine/core/math/*`;
  - scene serialization: `editor/Stream.cpp`, 8.6k lines;
  - `editor/command/CommandHistory.*`, including the play floor and `mark/groupSince`;
  - `editor/util/AssetDatabase.*`;
  - the CPU side of the renderer: `engine/core/shader/*` and `engine/core/render/PipelineState.h`.
- **MCP tests.** `tools/mcp/tests/` has 62 files and 549 tests. A run at `-j 4` takes about 3,900 s.
  - Most files use the default fixture `build/fixture-unity` (`tools/mcp/harness.py:25-27,145-153`). That fixture is **not in git**, so CI cannot use it.
  - Sixteen files create their own project (`EDITOR = {"create": True, ...}`) or use `tools/fixtures/*`.
  - `run-tests.sh` installs `mcp` without a version pin.
- **`tools/smoke-test.sh`** exports, builds and runs a project under Xvfb, then screenshots it.
  - It already treats `AddressSanitizer` lines in a log as fatal (`:100`).
  - It builds its image by tag (`:61-64`).
- **`.github/workflows/ci.yml`** builds Linux, Web and Android on every push. Windows and macOS build on demand, on PRs, on tags and on releases.
  - It runs **no tests at all**.
  - The Linux job installs its dependencies into a bare `ubuntu:22.04` container (`:43-75`). The web job uses `setup-emsdk` on the host (`:224-226`).
- **Web.** `cmake -S engine` under Emscripten emits `jasperengine-project.html`, which draws `engine/project/assets/jasperengine.png` (`engine/CMakeLists.txt:177-240`, `engine/project/main.cpp`).
  - Nothing ever runs it.
  - `JASPERENGINE_READY_SIGNAL` is read with `getenv` (`engine/core/Engine.cpp:1171`), which a browser page cannot set.
- **Version and notices.**
  - The version comes from `git describe --tags --match 'v[0-9]*'` (`CMakeLists.txt:6-24`, `ci.yml:82-83`).
  - `NOTICE.md` is not installed with the editor, and has no googletest row.

## 4. Reference (OpenEngine, MIT; read-only, never executed)

- **`CMakeLists.txt:839-890`, MSVC ASan runtime-DLL staging.** Port the pattern as `jasperengine_stage_asan_runtime(<target>)`.
- **`CMakeLists.txt:905-1024`, warning sets, Werror, ASan and TSan.**
  - Take: the per-compiler warning list, ASan on MSVC (strip `/RTC1`, force the DLL CRT, `/INCREMENTAL:NO`, `_DISABLE_STL_ANNOTATION`), the genex-quoting fix (`"$<...:-fsanitize=address;-fno-omit-frame-pointer>"`), the hard error when TSan and ASan are both on, and the error for TSan on MSVC.
  - Improve: apply warnings *per target* (they use global `add_compile_options`, which would also hit vendored libs and exported projects), use a single `JASPERENGINE_SANITIZE` list instead of separate booleans, add UBSan, and don't tie sanitizers to the Debug config only.
- **`CMakePresets.json`, unity presets, `SKIP_UNITY_BUILD_INCLUSION`** (`Engine/CMakeLists.txt:842,1204-1223`). Take only the shape: hidden base presets plus host/config leaves. Write our own short file.
- **`Tools/Web/serve.py`** (61 lines) and **`Tools/Web/browser_gate.py`** (581 lines, stdlib only).
  - Take: the minimal RFC 6455 CDP client, the COOP/COEP/no-store handler, `--headless=new`, the timestamped console transcript, the transcript-before-capture ordering (a wedged page still yields its console output), `Page.captureScreenshot` as the only capture path (their note: drawImage readback returns zeros), `CentrePixel`, `--expect`, and `--eval`.
  - Replace: the WebGPU/Dawn error markers with WebGL/sokol/engine markers.
  - Add: waiting for the engine's ready line, `Runtime.exceptionThrown`, and a baseline file.
  - Drop: `--wasm-memory`, `--reloads`, `--click` and `--step`.
  - Keep their MIT header plus a `NOTICE.md` row ("portions adapted from OpenEngine").
- **`CONTRIBUTING.md` §Tests, policy only.** Declared tests = ran + disabled, a filter that matches nothing is not a pass, a behavioural claim needs a red control, and gtest XML is kept as evidence.
- **`Tests/ECS/ECSCoreTest.cpp`** for the kinds of cases worth covering. Their ECS is on the do-not-copy list, so take test ideas only, not code.

## 5. Requirements

### Functional — build options

- **R1. `JASPERENGINE_SANITIZE`** is a cache string in both trees (root and `engine/`). It holds a list drawn from `address`, `undefined` and `thread`, and is empty by default.
  - Configure fails with a clear message for `thread` together with `address`, for `thread` or `undefined` on MSVC, and for any value on Emscripten or Android.
  - The flags apply to every target in the tree, vendored libraries included, because uninstrumented code gives TSan false positives:
    - GCC/Clang: `-fsanitize=<list> -fno-omit-frame-pointer`, plus `-fno-sanitize-recover=undefined -fno-sanitize=vptr,function` when `undefined` is in the list.
    - MSVC: `/fsanitize=address` with the OpenEngine MSVC fixes, plus runtime-DLL staging for every executable.
  - The option defines `JASPERENGINE_SANITIZE_ADDRESS`, `JASPERENGINE_SANITIZE_THREAD` and `JASPERENGINE_SANITIZE_UNDEFINED`, so that tests can relax time budgets under a sanitizer.
- **R2. Suppression files.** `tools/sanitizers/{lsan,ubsan,tsan}.supp` hold the suppressions, each entry with a comment giving its reason.
  - Only third-party code may be suppressed: Mesa/llvmpipe/lavapipe, X11, fontconfig and vendored libraries. Engine and editor code is never suppressed.
  - `tools/build.sh` exports the options for every run:
    - `ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:strict_string_checks=1`
    - `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:suppressions=…`
    - `LSAN_OPTIONS=suppressions=…`
    - `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=…`
- **R3. `JASPERENGINE_UNITY_BUILD`** (default OFF) sets `UNITY_BUILD ON` and `UNITY_BUILD_BATCH_SIZE 16` on our own targets only: `jasperengine`, `jasperengine-editor`, `-editor-cmd`, the project executable and the test targets. Vendored libraries are never unity-built in S02.
  - TUs that cannot be merged are fixed by preference: file-local helpers move into a uniquely named namespace.
  - Otherwise they are marked `SKIP_UNITY_BUILD_INCLUSION` with a one-line reason. `.mm`/`.m` files and generated sources are always skipped.
- **R4. Warnings.**
  - `JASPERENGINE_WARNINGS` defaults ON in the root (editor) tree and OFF in `engine/`. Exported projects must stay quiet, so the Exporter never sets it.
    - It applies `jasperengine_target_warnings(<target>)` from the new `cmake/JasperWarnings.cmake` to our own targets.
    - GCC/Clang flags: `-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare`.
    - MSVC flags: `/W3 /wd4244 /wd4267 /wd4305`.
  - `JASPERENGINE_WERROR` (default OFF; requires WARNINGS) adds `-Werror -Wno-error=deprecated-declarations`, or `/WX` on MSVC, to those same targets.
  - Vendored libraries keep `-w`/`/w`, and their headers stay `SYSTEM`.
  - The suppressed groups (sign-compare, MSVC conversions) are a documented ratchet in `docs/testing.md`. Turning them on is follow-up work, not part of S02.
- **R5. Presets.** `/CMakePresets.json` (editor tree) and `/engine/CMakePresets.json` (standalone engine tree) use preset schema version 6 with `cmakeMinimumRequired` 3.27.
  - They hold hidden bases plus the leaves in D3, together with matching `buildPresets` and `testPresets`.
  - Presets never hard-code ccache. `tools/build.sh` and CI pass the launcher on the command line.
  - `engine/CMakePresets.json` must not reach the editor install or exports. It is excluded in `CopyEditorEngine.cmake` and in the `install(DIRECTORY engine …)` rule.
- **R6. `tools/build.sh`** builds through the presets (`cmake --preset <p> -B <its historical dir>`), so output paths stay as they are and `smoke-test.sh` and the MCP harness keep working.
  - **New targets:**
    - `unit-tests`: builds and runs both unit-test binaries.
    - `mcp-tests`: runs `tools/mcp/run-tests.sh` in the Linux image.
    - `web-gate`: runs `browser_gate.py` in the web image against `build/web`.
  - **New environment variables:**
    - `SANITIZE=<list>`, honoured by `linux`, `unit-tests` and `htmlui-tests`. Sanitized builds go to `build/linux/<target>-<sanitizers>` directories so they never mix objects with plain builds.
    - `UNITY=1`.
    - `WERROR=1`.
    - `UNIT_TEST_ARGS`.
    - `JASPER_DOCKER_IMAGE=<id|tag>`, which skips `docker build`. `smoke-test.sh` honours it too, as CI's prebuilt-image hook.
  - **Test results** go to `build/test-results/<suite>.xml`, with MCP results as JSON.

### Functional — test framework

- **R7. googletest** 1.17.0 is vendored at `libs/googletest/` (BSD-3-Clause; `NOTICE.md` row marked "tests only, never shipped"), with gmock enabled.
  - `cmake/JasperTesting.cmake` adds it once per tree: `EXCLUDE_FROM_ALL`, `-w`.
  - It provides `jasperengine_add_test_target(<name> SOURCES … LABELS …)`, which:
    - registers the tests with CTest (`gtest_discover_tests(... DISCOVERY_MODE PRE_TEST)`);
    - sets `JASPER_TEST_SOURCE_DIR` and `JASPER_TEST_BINARY_DIR`;
    - applies warnings.
  - The two HTML UI targets switch to it. Their behaviour does not change.
- **R8. `JASPERENGINE_TESTS`** (default OFF), one name in both trees:
  - In `cmake -S engine` it builds **`jasperengine-core-tests`**, which links the static `jasperengine` and covers engine code.
  - In the root tree it builds **`jasperengine-editor-tests`**, which covers editor logic. It links the editor sources minus `main.cpp`, using the object-library pattern of `HtmlUiEditorTests.cmake` so the editor objects stay ccache-shared.
  - Both targets build and their CPU suites run on Linux (GCC), Windows (MSVC) and macOS (AppleClang). GPU suites compile and run on Linux only.
- **R9. Layout.**
  - `engine/tests/support/` is the static library `jasperengine-test-support`. It holds what moves out of `engine/tests/htmlui/harness/`: `TestSystem`, `GpuEnvironment`, `HeadlessContext`, `Readback` and `ImageCompare`.
    - Each moved header leaves a one-line forwarding header at its old path, so no HTML UI test file changes.
    - The asset root of `TestSystem` becomes a per-target compile definition (`JASPER_TEST_ASSET_DIR`).
    - New pieces: `TempDir` (RAII, unique per test), `LogCapture` (with `EXPECT_NO_ENGINE_ERRORS()`) and a shared `TestMain.cpp`. `TestMain.cpp` initialises the engine without a GPU, as `engine/tests/htmlui/main.cpp` does today.
  - `engine/tests/core/<area>/` mirrors `engine/core/<area>/` (`ecs/`, `math/`, `registry/`, `shader/`, `render/`, `util/` …), with fixtures in `engine/tests/core/fixtures/` and golden images in `engine/tests/core/golden/`.
  - `editor/tests/<area>/` mirrors `editor/` (`command/`, `stream/`, `util/` …), with fixtures in `editor/tests/fixtures/`.
  - `engine/tests` is already excluded from the SDK copy. `editor/tests` is never installed.
- **R10. Naming.**
  - Files are named `<Subject>Test.cpp`.
  - Suites are named `<Subject>`, using `TEST`/`TEST_F(Subject, VerbPhrase)` with CamelCase test names.
  - Suites that need a GPU end in `Gpu` and start with `JASPER_REQUIRE_GPU()`. They fail without a GPU; they never skip.
  - Suites over 5 s per test end in `Slow`.
  - CTest labels: `unit`, `gpu`, `slow`.
- **R11. No silent green.** `tools/ci/gtest-gate.py --xml F [--min-tests N] [--max-skipped K]` fails in any of these cases:
  - zero tests ran (a filter matched nothing);
  - any test failed or errored;
  - skipped tests exceed K. K is 0 for the core and editor targets, and the recorded count of 74 for `jasperengine-htmlui-tests`.
  - It also prints the disabled tests. Every CI test step goes through it.
- **R12. First batch of tests.** Section 7 lists the minimum counts. These are *characterization* tests: they pin today's behaviour. Specs that change that behaviour on purpose (S07: the 64-type limit, O(1) add/remove) update the affected tests in the same change and say so.
  - **ECS:** `ComponentArray` (insert, remove keeps dense packing, `entitiesDestroyed` batch, `sortByEntityOrder`, `moveEntityToIndex`), `EntityManager` id reuse, `EntityRegistry` add/get/remove/has, signature updates on add/remove, `destroyEntity` drops every component, `Scene` hierarchy order after reparenting, and the 64-component-type limit asserted as the current contract.
  - **Math:** `Vector2/3/4` operators and normalisation, `Quaternion` (axis-angle, slerp end points, to/from `Matrix4`/`Matrix3`, inverse), `Matrix4` (multiply, inverse × self ≈ identity, `lookAt`, perspective and orthographic against hand-computed values), `AABB`/`OBB`/`Ray`/`Plane`/`Sphere` intersection cases, and `Angle`.
  - **Serialization (editor `Stream`):**
    - every enum `xToString`/`stringToX` pair round-trips every enumerator, and an unknown string gives the documented default;
    - `encode`/`decode` of `Vector*`, `Quaternion`, `Matrix4` and `Transform` round-trip;
    - for each component type `Stream` handles, an entity carrying it re-encodes to identical YAML (encode → decode → encode);
    - `encodeScene`/`decodeScene` round-trip;
    - every `.scene` file under `editor/tests/fixtures/scenes/` loads with no logged error and re-encodes stably. The scenes are copied from `tools/fixtures/*`, and S03 adds its version-migration fixtures here.
  - **CommandHistory**, with a recording fake `Command`: undo/redo order, `canUndo`/`canRedo`, merging and `setNoMerge`, a redo tail dropped by a new command, `mark`/`groupSince` (including the no-op cases in `CommandHistory.h:45-49`), `beginPlaySession`/`endPlaySession(true|false)` floors and the restored redo tail, and `onSceneModified` arguments.
  - **AssetDatabase**, on a `TempDir` project:
    - `generateGuid`/`isGuid`, `writeGuid`/`readGuid`, and `legacyGuid` stability;
    - `open`, `refresh` (Added, Removed, Moved and Modified `Change`s), `moveAssets`, `removeAssets` and `importAssets`;
    - a duplicated GUID gets a new GUID on refresh;
    - the cache survives `close`/`open`.
    - S04 extends these tests.
  - **Renderer:**
    - CPU suites: `ShaderManifest` parsing, a `ShaderDataSerializer` round-trip, `ShaderVariantCollection` key building, and `PipelineState` equality and hash.
    - GPU suites (`RenderGpu`, Linux, glcore and Vulkan): an unlit coloured `Primitive` plane and a lit cube rendered into `GpuEnvironment::target()` and compared to two goldens in `engine/tests/core/golden/` through `ImageCompare`. `JASPER_TEST_UPDATE_GOLDEN=1` re-baselines them.
    - These are the first 3D-renderer goldens. S10–S13 add theirs here.

### Functional — CI and the web gate

- **R13. The CI workflow** (`ci.yml`) adds Linux jobs that run on every push and PR. Each one builds the images with `docker/build-push-action` (GHA cache, `load: true`), passes `JASPER_DOCKER_IMAGE`, caches `build/.cache/ccache` with `actions/cache`, uploads `build/test-results/**` always, and uploads `build/smoke/**` and the golden diff images on failure. The jobs are:
  - `unit`: `WERROR=1 tools/build.sh unit-tests` (GCC).
  - `sanitizers`: `SANITIZE=address,undefined tools/build.sh unit-tests`, with GPU suites included under Xvfb.
  - `htmlui`: `tools/build.sh htmlui-tests` (glcore), with `JASPER_HTMLUI_PERF_SLACK` set for shared runners.
  - `e2e`:
    1. `tools/build.sh linux` (editor plus engine)
    2. `tools/build.sh htmlui-editor-tests`
    3. `MCP_SUITE=ci tools/build.sh mcp-tests -j 2`
    4. `tools/smoke-test.sh tools/fixtures/post-processing` (OpenGL)
  - `web`: replaces the host emsdk build with `tools/build.sh web` (Debug), then `tools/build.sh web-gate`.
  - Existing build, packaging and release jobs keep their behaviour.
  - Windows and macOS jobs (still on demand) build with `JASPERENGINE_TESTS=ON` and `JASPERENGINE_WERROR=ON`, then run both unit binaries with `--gtest_filter=-*Gpu*` through the gate.
- **R14. The nightly workflow** (`nightly.yml`: `schedule` once a day plus `workflow_dispatch`, on `main` and `oe-port`) runs:
  - `htmlui-tests` on Vulkan;
  - `htmlui-tests` with ASan+UBSan, minus `*Golden*`;
  - `SANITIZE=thread tools/build.sh unit-tests`, minus `*Gpu*`;
  - the `UNITY=1` builds of the editor and the engine;
  - the smoke test of every `tools/fixtures/*` project with OpenGL, plus the GLES3 and Vulkan sets listed in `phase-6-acceptance.md` §4.4;
  - the threaded web build (`EMSCRIPTEN_THREAD_SUPPORT=ON`) through the gate.
  
  A failure opens or updates one GitHub issue labelled `nightly`, through `gh`.
- **R15. MCP CI suite.**
  - A test file joins the CI suite by declaring `CI = True` at module level.
  - `run_tests.py --suite ci` runs exactly those files. It fails (exit 2) when a `CI = True` file relies on the default fixture, meaning it has neither `EDITOR["create"]` nor an explicit `project`.
  - The initial members are the files that already create their own project or use `tools/fixtures/*` (16 today), plus `test_protocol.py`.
  - New MCP tests from other specs are written CI-capable by default.
  - `tools/mcp/requirements.txt` pins `mcp` and `pyyaml`, and `run-tests.sh` uses `--with-requirements`.
  - The Linux image gains a pinned `uv` (in its own layer).
- **R16. `tools/web/serve.py`** serves with `Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp`, `Cache-Control: no-store`, and the `application/wasm` and `.data`→`application/octet-stream` MIME types.
- **R17. `tools/web/browser_gate.py`** (stdlib only). Its arguments:
  - `--root DIR` and `--page FILE`;
  - `--until-ready` (wait for `Engine ready:`), `--timeout S` (120), `--seconds S`;
  - `--screenshot PNG`, `--baseline JSON`, `--expect TEXT` (repeatable), `--eval JS`.
  
  It launches Chrome headless with `--use-angle=swiftshader --enable-unsafe-swiftshader`, through a throwaway profile, and opens the page with `?jasperengine_ready_signal=1`. It prints:
  - the timestamped console transcript;
  - `Runtime.exceptionThrown` entries;
  - `crossOriginIsolated`;
  - the centre pixel.
  
  It exits non-zero in any of these cases:
  - an exception;
  - a console error-level message;
  - a line matching `WebGL: INVALID_`, `GL_INVALID`, `(ERROR)`, `[ERROR]`, `Lua Error`, `[Mm]issing (precompiled )?shader`, `Aborted(` or `RuntimeError`;
  - no ready line before the timeout (with `--until-ready`);
  - a missing `--expect` line;
  - a centre pixel outside the baseline's `rgb ± tolerance`;
  - `crossOriginIsolated` differing from the baseline's `isolated`.
  
  The baselines live in `tools/web/gates/*.json`: `sample-project.json` and `sample-project-threads.json`.
- **R18. Web ready signal.** The web platform (`engine/platform/emscripten/main.cpp`) sets `JASPERENGINE_READY_SIGNAL=1` with `setenv` before engine init when the page URL carries `jasperengine_ready_signal=1`. This is the only engine source change in S02.
- **R19. Chrome in the web image.** `docker/web.Dockerfile` installs a pinned Chrome for Testing `chrome-headless-shell`, checks its sha256, and adds its runtime libraries, all in its own layer. On the host, `JASPER_CHROME` overrides the browser path.
- **R20. Release tag check.** Before uploading, the release job fails unless the tag matches `^v[0-9]+\.[0-9]+(\.[0-9]+)?$` and `git describe --tags --match 'v[0-9]*' --exact-match` at the built commit equals the tag.
- **R21. Notices.**
  - `NOTICE.md`, `LICENSE` and a generated `licenses/` folder (each vendored lib's licence file, named `<lib>-LICENSE`) are installed next to the editor binary, so they end up in the zips, the AppImage and the dmg's `Resources`.
  - `tools/ci/check-notices.py` fails CI when any directory under `libs/`, `engine/libs/` or `engine/libs/shadercompiler/libs/` lacks a `NOTICE.md` row or a licence file. It runs in the `unit` job.

### Platform & backends

- **R22.** Every new CMake option configures cleanly, with the option OFF, for every export target: Windows, Linux, macOS, Android (gradle), iOS (Xcode workspace) and Emscripten. The existing CI jobs prove this. The test targets are desktop-only.

### Performance budgets

- **R23.** Without GPU suites, `jasperengine-core-tests` plus `jasperengine-editor-tests` run in ≤ 60 s on 4 cores. A test outside a `Slow` suite takes ≤ 1 s.
- **R24.** CI wall time with a warm ccache:
  - `unit` ≤ 25 min;
  - `sanitizers` ≤ 35 min;
  - `htmlui` ≤ 45 min;
  - `e2e` ≤ 60 min;
  - `web` ≤ 30 min;
  - PR critical path ≤ 60 min.
  
  The MCP CI suite takes ≤ 20 min at `-j 2`.
- **R25.** Unity build: a cold build of the editor plus engine in the Linux image (`JOBS=8`) must be ≥ 25 % faster than the non-unity build. If it is not, the option stays but no CI job or preset recommends it (`docs/testing.md` records the measurement).

### Developer & agent UX

- **R26. `docs/testing.md`** covers:
  - every suite, and how to run it locally with `tools/build.sh`;
  - naming and layout (R9, R10);
  - the no-silent-green rules and the red-control rule;
  - golden re-baselining;
  - the sanitizer workflow;
  - the warning ratchet;
  - how a spec adds tests.
  
  `tools/build.sh` usage lists the new targets. The MCP agent guide (`docs/mcp/README.md`) documents `CI = True`. S31's AGENTS.md links `docs/testing.md`.

### Data & compatibility

- **R27.** No runtime, scene or asset format changes. Exported projects configure and build exactly as before: no new warnings, no Werror, no sanitizer, no presets file.

## 6. Design constraints & decisions

- **D1. Options are per tree, and safe by default.**
  - `engine/CMakeLists.txt` is a user-facing project file, so every option added there defaults to off or empty.
  - Warnings and Werror attach to targets through `jasperengine_target_warnings()`, never through `add_compile_options`.
  - Sanitizers are deliberately global (R1).
- **D2. Two unit-test binaries, not one.** The editor's shared engine hides internal symbols (`engine/CMakeLists.txt:780-783`), and the editor needs ImGui and editor state. Engine code is therefore tested against the static engine, and editor code inside the editor tree. They share `jasperengine-test-support` and `TestMain.cpp`.
- **D3. Preset leaves.** Binary dir: `build/presets/<name>`. `tools/build.sh` overrides it with `-B`.
  - Root tree:
    - `linux-release`, `linux-debug`, `linux-vulkan-release`
    - `linux-asan` (Debug, `address;undefined`), `linux-tsan` (Debug, `thread`)
    - `linux-unity`
    - `linux-ci` (RelWithDebInfo, `JASPERENGINE_TESTS`, `JASPERENGINE_WERROR`)
    - `windows-msvc-release`, `windows-msvc-debug`, `windows-msvc-asan` (Ninja; needs the MSVC environment)
    - `windows-vs2022` (Visual Studio generator, for the IDE)
    - `macos-release`, `macos-universal-release`, `macos-asan`
  - `engine/` tree:
    - `engine-linux-release`, `engine-linux-debug`
    - `engine-linux-tests` (RelWithDebInfo, `JASPERENGINE_TESTS`, `JASPERENGINE_WARNINGS`, `JASPERENGINE_WERROR`), `engine-linux-tests-vulkan`, `engine-linux-asan`, `engine-linux-tsan`
    - `engine-windows-msvc-tests`, `engine-macos-tests`
    - `engine-web-debug`, `engine-web-release`, `engine-web-threads`. These use `$env{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake` as the toolchain, which the web image provides.
  - Android stays on gradle and iOS on the Xcode workspace; neither gets a preset.
- **D4. Build types.**
  - Unit tests: RelWithDebInfo. It is fast and debuggable, and its objects are distinct from the Release cache anyway.
  - Sanitizers: Debug, so assertions are live.
  - HTML UI: stays Release.
- **D5. googletest: vendor 1.17.0, don't keep borrowing yaml-cpp's 1.11 test tree.** Updating yaml-cpp would otherwise silently move or delete our framework. gmock is now available for faking `System`, file I/O and similar.
- **D6. CI runs the same Docker images and `tools/build.sh` as the remote build servers.** That gives one place to reproduce a failure. The `linux` packaging job keeps its bare `ubuntu:22.04` container, because the AppImage glibc floor is defined there; S01 aligns its compiler with the images.
- **D7. Werror is enforced on** Linux GCC (images: `unit`, `e2e`), MSVC (`windows` job) and AppleClang (`macos` CMake builds). **It is never enforced on** Emscripten, the Android NDK, the Xcode workspace, or exported projects: those compilers move under us and the code includes user code. Warnings there are reported only.
- **D8. Warning cleanup plan.**
  - **W0:** land `JASPERENGINE_WARNINGS` with Werror off. CI writes each target's `warning:` count to the job summary.
  - **W1:** fix GCC warnings in engine, editor and tests to zero, as separate mechanical commits listed in `.git-blame-ignore-revs` where they are large. Then turn `WERROR=1` on in `unit` and `e2e`.
  - **W2:** the same for MSVC `/W3`, then AppleClang. Then turn `JASPERENGINE_WERROR=ON` on in the windows and macos jobs.
  - W0–W2 all belong to S02. Changes in flight in other streams must compile warning-free once W1 lands; that is the merge rule.
- **D9. No VERSION file.** `git describe` stays the single source (`CMakeLists.txt:6-24`), and the release tag check (R20) gives the guarantee a VERSION file would. Revisit only if S30 needs a version independent of git.
- **D10. Notices are staged into the editor now and into exported games in S30.** `check-notices.py` is the shared validator.
- **D11. The web gate runs against the engine sample build.** It needs no editor and no exporter. S30 adds an exported-fixture gate with the same tool and baselines format.
- **Sketches:**

```cmake
# cmake/JasperWarnings.cmake
function(jasperengine_target_warnings target)   # no-op unless JASPERENGINE_WARNINGS
# cmake/JasperSanitizers.cmake — included first by both trees
set(JASPERENGINE_SANITIZE "" CACHE STRING "address;undefined | thread | empty")
function(jasperengine_stage_asan_runtime target) # MSVC only
# cmake/JasperTesting.cmake
function(jasperengine_add_test_target name)       # SOURCES, LABELS, LINK, ASSET_DIR
```

```
tools/build.sh unit-tests        [SANITIZE=address,undefined|thread] [WERROR=1] [UNIT_TEST_ARGS=--gtest_filter=…]
tools/build.sh mcp-tests         [MCP_SUITE=ci|all] [MCP_TEST_ARGS=…]   (all needs JASPER_FIXTURE)
tools/build.sh web-gate          [GATE=sample-project|sample-project-threads]
python3 tools/ci/gtest-gate.py --xml build/test-results/core.xml --min-tests 300 --max-skipped 0
python3 tools/web/browser_gate.py --root build/web --page jasperengine-project.html \
    --until-ready --baseline tools/web/gates/sample-project.json --screenshot build/test-results/web/sample.png
```

## 7. Acceptance criteria

Each criterion names how it is verified.

- **AC1.** `cmake --list-presets` lists the D3 leaves in both trees. In the Linux image, each `linux-*` and `engine-linux-*` preset configures. *(Verified by a `tools/ci/check-presets.sh` step in `unit` that configures each with `--fresh`.)*
- **AC2.** `SANITIZE=address,undefined tools/build.sh unit-tests` is green, and the binary links `libasan` and `libubsan` (`ldd`). A planted heap-use-after-free test, run once as a red control and recorded in `docs/testing.md`, makes the run fail with `AddressSanitizer`. The same holds for TSan with a planted data race.
- **AC3.** Configuring with `JASPERENGINE_SANITIZE="address;thread"` fails with the R1 message. Configuring an Emscripten tree with any sanitizer fails as well. *(`tools/ci/check-presets.sh` asserts the non-zero exit.)*
- **AC4.** The `windows-msvc-asan` preset builds, and `jasperengine-core-tests` runs from a plain shell with `clang_rt.asan_dynamic-x86_64.dll` staged next to it. *(Manual check on a Windows dev box, recorded in `docs/testing.md`.)*
- **AC5.** `UNITY=1 tools/build.sh linux` and the unity engine build succeed (nightly job). The R25 measurement is recorded in `docs/testing.md`.
- **AC6.** After W1, `WERROR=1 tools/build.sh unit-tests` and `WERROR=1 tools/build.sh linux` build with 0 warnings from our targets. After W2, the windows and macos jobs build with `JASPERENGINE_WERROR=ON`. In an exported project's `export-build/CMakeCache.txt`, `JASPERENGINE_WERROR` and `JASPERENGINE_SANITIZE` are absent or OFF/empty. *(A check added to `smoke-test.sh`.)*
- **AC7.** `tools/build.sh unit-tests` runs both binaries and writes `build/test-results/{core,editor}.xml`, and the gate passes:
  - `core.xml`: ≥ 300 tests, 0 skipped;
  - `editor.xml`: ≥ 150 tests, 0 skipped.
- **AC8.** The minimum counts of the first batch:

  | Suite | Minimum tests |
  |---|---|
  | ECS + registry | ≥ 60 |
  | Math | ≥ 80 |
  | Stream | ≥ 100, with every enum pair and every component type Stream encodes covered |
  | CommandHistory | ≥ 25 |
  | AssetDatabase | ≥ 25 |
  | Renderer CPU | ≥ 30 |
  | `RenderGpu` | 2 golden tests, passing on glcore and Vulkan |

  *(Verified with `--gtest_list_tests` counts in `docs/testing.md`.)*
- **AC9.** Each first-batch area has one recorded red control: the behaviour reverted locally, the test failing, the output pasted in the PR.
- **AC10.** `gtest-gate.py` exits non-zero on: an XML file with 0 tests, a `--gtest_filter` that matches nothing, one failure, or more skips than allowed. *(`tools/ci/test_gtest_gate.py`, a Python unittest with XML fixtures, run in `unit`.)*
- **AC11.** `jasperengine-htmlui-tests` and `jasperengine-htmlui-editor-tests` keep their exact test counts after switching to `libs/googletest` and `engine/tests/support`: 3,311 declared, 74 skipped, and 73 tests respectively. No golden changes.
- **AC12.** On a PR, `unit`, `sanitizers`, `htmlui`, `e2e` and `web` all run, and the PR is blocked if any of them fails. *(Shown by a throwaway PR with a deliberately failing core test.)* All five finish within the R24 budgets: the job durations of 5 consecutive green runs are recorded in `docs/testing.md`.
- **AC13.** `run_tests.py --suite ci` runs ≥ 17 files and ≥ 120 tests with 0 failures. Adding `CI = True` to a file that relies on the default fixture makes the run exit 2. *(Case added to `tools/mcp/selftest.py`.)*
- **AC14.** `tools/smoke-test.sh tools/fixtures/post-processing` passes in `e2e`, and `game.png` and `editor.png` are uploaded as artifacts.
- **AC15.** `tools/build.sh web-gate` passes in CI:
  - the ready line appears;
  - 0 errors;
  - the centre pixel is within the baseline;
  - `crossOriginIsolated` is `true` (served with COOP/COEP).
  
  It fails when a planted `Log::error` is added to `engine/project/main.cpp` (red control), and when the page is served without COOP/COEP against the threads baseline (nightly variant).
- **AC16.** The release job refuses a test release whose tag is `v1.2.3-bad`, or whose tag points at a commit other than the one built. *(Dry run with `workflow_dispatch` inputs recorded in the PR.)*
- **AC17.** The installed editor tree contains `NOTICE.md`, `LICENSE` and `licenses/<lib>-LICENSE` for every vendored lib, and no `engine/CMakePresets.json`. `check-notices.py` fails when a row is removed from `NOTICE.md`. *(`tools/ci/test_check_notices.py`.)*
- **AC18.** The nightly workflow runs green once end to end. A forced failure opens or updates the `nightly` issue.

## 8. Dependencies & conflicts

- **Depends on S01 (C++20).** S01 picks the compiler in the images (GCC 13), and the Werror cleanup (W1) must run on that compiler. Rule: W0 and the test framework can land on either; W1 waits until S01 is merged.
- **Blocks every spec's "tests" clause.**
  - **S03:** scene version fixtures in `editor/tests/fixtures/scenes`.
  - **S04:** extends the AssetDatabase tests.
  - **S06:** job system tests under TSan.
  - **S07:** updates the ECS characterization tests it intentionally changes.
  - **S08:** reflection must keep the Stream round-trip tests green.
  - **S09:** logger and events tests.
  - **S10–S13, S15:** add `RenderGpu` goldens.
  - **S24:** extends the CommandHistory tests.
  - **S30:** exported-fixture web gate, notices in exports.
  - **S31:** AGENTS.md test commands.
- **Merge hotspots:**
  - `.github/workflows/ci.yml`;
  - `tools/build.sh`;
  - `CMakeLists.txt` (root, near the option and include blocks) and `engine/CMakeLists.txt`. Those two are also touched by S01, S05, S06, S14 and S30;
  - `CopyEditorEngine.cmake`;
  - `docker/linux.Dockerfile` and `docker/web.Dockerfile` (S01 compiler, S14 encoders). Always add new layers at the end;
  - `NOTICE.md`;
  - `engine/tests/htmlui/harness/*` (S16, S17), which shrinks to forwarding headers;
  - `tools/mcp/run_tests.py`.
  
  Land the R9 harness move early in Wave 0 so that the HTML UI streams rebase onto it once.

## 9. Risks & defaults

- **The biggest risk is CI time and flakiness on shared 4-vCPU runners.** llvmpipe golden tests, the editor under Xvfb, and MCP timeouts can turn the gate red for reasons unrelated to the change.
  - Mitigations: `JASPER_HTMLUI_PERF_SLACK`, MCP `-j 2`, and Vulkan, TSan and the full smoke runs moved to nightly.
  - Default: a test that flakes twice in a week is quarantined with a `DISABLED_` prefix and a tracking issue. `gtest-gate.py` lists every disabled test, so quarantine is visible, never silent.
- **The warning count is unknown until W0 measures it.** Default: if GCC W1 exceeds about 1,500 warnings, add `-Wno-<group>` for the top group, document it in the ratchet, and still land Werror.
- **LSan noise from Mesa.** Default: GPU suites under ASan run with the `lsan.supp` third-party entries. If those are still noisy, GPU suites run with `detect_leaks=0`, and leak checking stays on for CPU suites.
- **Pinning Chrome for Testing:** pinned by version and sha256, and bumped by hand. If SwiftShader WebGL2 fails in the container, fall back to the runner's preinstalled Chrome on CI only and record the change.
- **`build/fixture-unity` cannot be published** (its licence is unknown). The MCP suite stays split: CI files (`CI = True`) plus the full suite on the build servers. Default: no attempt to commit it.
- **The forwarding headers left by the harness move** may confuse include-what-you-use. They are deleted once S16 and S17 have rebased.

## Deviations

Recorded by the S02 implementation.

- **CMake modules live in `engine/cmake/`, not `cmake/`.** `JasperSanitizers.cmake`, `JasperWarnings.cmake`, `JasperUnityBuild.cmake` and `JasperTesting.cmake` are included by both trees, and the standalone engine tree (`cmake -S engine`, every export) must find them inside `engine/`; S01's `JasperEngineToolchain.cmake` sits there too. They are inert in the SDK copy and in exports (every option defaults off there; `JasperTesting.cmake` is only included with a test option on).
- **`Stream` befriends `StreamTestAccess`.** Most of `Stream`'s enum and value conversions are private; one `friend class StreamTestAccess;` in `editor/Stream.h` lets `jasperengine-editor-tests` round-trip all of them (R12) without making them public.
- **Component round trips are per component.** Every component type `Stream` encodes is checked by encode → decode → encode of the component itself (`StreamComponent.*`), which isolates a field that is written but not read back; whole entities are round-tripped by `StreamScene.*` and by the 17 fixture scenes.
- **`jasperengine-htmlui-editor-tests` keeps `gtest_main`.** It moved to `jasperengine_add_test_target` (`EDITOR GTEST_MAIN`) and `libs/googletest`, but does not take `TestMain.cpp`: those tests never initialised an engine, and R7 says their behaviour does not change.
- **The test support library has its headless GPU in the standalone engine tree on Linux only.** In the editor tree (shared engine) and on Windows/macOS, `GpuEnvironment::ensure()` fails with a reason (GPU suites are Linux-only, R8/R10); everything else of the library builds everywhere.
- **Golden images and per-target directories go through `TestPaths`.** The support library is built once per tree; `TestMain.cpp`, compiled into each target with its `JASPER_TEST_*` definitions, sets the paths at start-up. `JASPER_TEST_UPDATE_GOLDEN=1` is the switch; the HTML UI suite's `JASPER_HTMLUI_UPDATE_GOLDEN=1` still works.
- **W2 cannot be completed from the build servers.** MSVC and AppleClang builds only happen in CI's Windows and macOS jobs. Both jobs now build with the warnings on (the editor tree by default, `-DJASPERENGINE_WARNINGS=ON` for the engine tree), run the unit tests, and write their warning counts to the job summary (`tools/ci/warning-summary.py`); `JASPERENGINE_WERROR=ON` is switched on there in the change that brings those counts to zero, after the first such CI run.
- **Acceptance steps that need GitHub or a Windows machine are prepared, not run:** AC4 (MSVC ASan by hand), AC12 (a throwaway PR and five timed runs), AC16 (the release job's dry run: `workflow_dispatch` input `check_release_tag`) and AC18 (a nightly run). Every workflow step is a script that was run on build server B (see the record in `docs/testing.md`), and the workflow files pass `actionlint`.
- **Additions to `tools/build.sh`:** `exec <target> <command>` (a non-interactive command in a build image, used by CI for `tools/ci/check-presets.sh`), `UNIT_SUITE=core|editor`, `WEB_THREADS=1` (the pthreads web build into `build/web-threads`) and `GATE_ARGS`.
- **The web gate reads the canvas's centre pixel**, not the page's: the default Emscripten shell does not centre the canvas in the window.
- **The nightly GLES3 and Vulkan smoke runs build their images themselves.** `docker/linux-backends.Dockerfile` builds `FROM jasperengine/linux-build`, which a BuildKit builder cannot take from the local image store; nightly time is not on the critical path.
- **The release tag check asks for the tags at the commit, not `git describe --exact-match`.** With two version tags on one commit (`v1.2.3` and a bad `v1.2.3-bad`, the AC16 case), `describe` returns one of them, and the good tag would fail the check. `tools/ci/check-release-tag.sh` requires the tag to match the R20 pattern and to be one of `git tag --points-at <commit> --list 'v[0-9]*'`.
- **Unity build: renames where few, group skips where pervasive.** In a unity unit every `namespace {}` of the batch is one namespace, and most of our sources open theirs at global scope after a `using namespace`, so moving helpers into a named namespace plus a using-directive would collide in the next file just the same. The editor's colliding helpers got a per-file name (`scenesDialogWidth`, `frameMonotonicSeconds`, `makePostProcessingPresets`, `LuaScriptOutline`, ...). The HTML UI sources (`engine/core/htmlui/`, `editor/htmlui/`) and the MCP tools (`editor/mcp/tools/`) keep one generic helper set per file (`lower`, `trim`, `startsWith`, `PI`, `Registry`, `Target`, `sceneArg`: dozens of files), so they are `SKIP_UNITY_BUILD_INCLUSION` as a group, like the sources that include Xlib (its `None`, `Bool`, `Status` macros), all through `jasperengine_skip_unity_build(REASON ... SOURCES ...)`.
- **TSan runs without ASLR.** On kernels with `vm.mmap_rnd_bits=32` (the build servers, current GitHub runners) a GCC 11 TSan binary aborts with `unexpected memory mapping`; `tools/build.sh` starts the test binaries under `setarch x86_64 -R` when `SANITIZE` has `thread`.
- **S03's interim test targets are folded in.** S03 landed first with stand-in `jasperengine-core-tests`/`jasperengine-editor-tests` targets (yaml-cpp's googletest 1.11) and its own `unit-tests` case in `tools/build.sh`. Its tests now build in the S02 targets (`editor/tests/support/*.cpp` joins the editor glob; `JASPER_TEST_REPO_DIR` and `JASPER_TEST_FIXTURES_DIR` are passed for its corpus and fixtures; `UNIT_SUITES` is still accepted). Two of its tests changed: braces around single assertions (`-Wdangling-else` under `WERROR`), and `UserSettingsTest` uses the binary's `TestSystem` (a user-data override and a sync counter) instead of calling `Engine::systemInit` with a `System` of its own, which left the log capture of every later test reading the wrong object. The data-safety lints (`tools/check_raw_writes.py`, `tools/check_stream_keys.py`) run in the `unit` job.
- **R13 and R14 are superseded by the CI design of 2026-11-12** (`docs/superpowers/specs/2026-11-12-ci-design.md`). The workflows they specified were deleted unrun: the account had no Actions billing. The repository is now public on an organization, where hosted runners cost nothing and a merge queue exists, so the replacement differs in three ways. Its jobs are lanes planned per diff by `tools/check-changed` and gated by one required `Check`, instead of the fixed `unit`/`sanitizers`/`htmlui`/`e2e` jobs on every push. Build images come from GHCR, because a `type=gha` layer cache of the web image alone would take 40% of the 10 GB Actions cache. Windows and macOS (and iOS) build and test on every PR and in the queue instead of on demand, because "the engine does not build" on a platform is the failure that hurts most. R14's nightly configurations carry over to its `nightly.yml`.
