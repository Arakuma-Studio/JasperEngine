# Testing JasperEngine

Every change ships with tests, and every suite runs the same way on a developer machine, a build
server and CI: inside the Docker images under `docker/`, through `tools/build.sh`. This page lists
the suites, how to run them, where a new test goes, and the rules that keep a green run honest.
The design and acceptance record is `docs/specs/S02-build-ci-tests.md`.

## The suites

| Suite | What it covers | Run it | Results |
|---|---|---|---|
| `jasperengine-core-tests` | engine code (`engine/core`, `engine/renders`) against the static engine: ECS, math, registry, shaders, render state, utilities, the job system, allocators, asset I/O, the pose animation core (`pose/`) and the animation clips core (`animation/`: clip IO, sampling, binding, the clip pool and the Animation Player, on a temporary asset root around each of its tests); `*Gpu` suites render into a headless device | `tools/build.sh unit-tests` | `build/test-results/core.xml` |
| `jasperengine-editor-tests` | editor logic (`editor/`): scene serialization (`Stream`), undo (`CommandHistory`, the S24 suites `CommandHistoryTests`, `UndoTimelineTests`, `PrefabUndoTests`, `PlayChangeReviewTests`, `CommandHistoryBench`), the asset database, Physics Placement, FBX import, texture synthesis, the Pose Animation window's logic and templates (`pose/`), animation clip documents, the clip controller, Record and the converters (`animation/`); and in `editor/tests/engine/` engine code driven through the editor's shared engine (physics sync and shapes, placement worlds, convex decomposition, actions, shader and texture keys, water, terrain) | `tools/build.sh unit-tests` | `build/test-results/editor.xml` |
| `jasperengine-htmlui-tests` | the HTML/CSS UI, with its golden images | `tools/build.sh htmlui-tests` (`HTMLUI_BACKEND=vulkan`) | `build/test-results/htmlui-<backend>.xml` |
| `jasperengine-htmlui-editor-tests` | the HTML UI importers against a local HTTP fixture server | `tools/build.sh htmlui-editor-tests` | `build/test-results/htmlui-editor.xml` |
| MCP tests | the editor's MCP server, driven like an agent (`tools/mcp/tests/test_*.py`) | `tools/build.sh linux`, then `tools/build.sh mcp-tests -j 2` | `build/test-results/mcp.json` |
| Benchmarks | the job system's and the allocators' budgets (S06 R23-R26): `je-bench-jobs`, `je-bench-memory`, gated by `tools/perf/check_bench.py` against `tools/perf/bench-thresholds.json` | `docs/jobs-memory.md` (on an otherwise idle machine) | their `--json` files |
| Smoke test | export, build and run a project, then open it in the editor; screenshots | `tools/smoke-test.sh tools/fixtures/post-processing` | `build/smoke/` |
| Upgrade from origin/main | a game an origin/main editor made (`tools/test-projects/upgrade-from-main`) opened, migrated, played, exported for Linux and the web in this editor, against the old editor's transcripts ([upgrading-from-main.md](upgrading-from-main.md)) | `tools/build.sh linux`, then `tools/ci/upgrade_from_main/check.sh` (the editor part also in the MCP suite: `test_upgrade_from_main.py`) | `build/upgrade-from-main/check/report.md` |
| Web gate | the web build in headless Chrome (WebGL2 on SwiftShader) | `tools/build.sh web`, then `tools/build.sh web-gate` | `build/test-results/web/` |
| Android render gate | an APK on the Android emulator: logcat, GLES errors, and the screen against goldens (a lit 3D scene, 2D/UI, HTML UI) | `ANDROID_PROJECT=render-gate ANDROID_ABIS=x86_64 tools/build.sh android`, then `tools/android-emulator-test.sh` (below; KVM servers A and C) | `build/android-emulator/<scene>/` |
| Crash diagnostics | the crash handler's report folders, sessions, hook and ring, the hang watchdog (`HangWatchdog.*`, `Crash*` in `jasperengine-core-tests`, which start `jasperengine-crash-test`, `engine/tests/crash/CrashTestMain.cpp`), the symbolication tool, the web crash hooks | `tools/build.sh unit-tests`; `python3 -I tools/crash/test_symbolicate.py <jasperengine-crash-test>`; `WEB_PROJECT=crash-diagnostics tools/build.sh web`, then `GATE=crash-diagnostics tools/build.sh web-gate` | as above |
| Tool checks | the CI scripts' own tests, the notices, the presets, the HTML UI golden ledger | `python3 tools/ci/test_gtest_gate.py`, `python3 tools/ci/test_android_screen_check.py`, `python3 tools/ci/test_check_notices.py`, `python3 tools/ci/check-notices.py`, `python3 tools/ci/test_check_golden_ledger.py`, `python3 tools/htmlui/check_golden_ledger.py`, `tools/build.sh exec linux tools/ci/check-presets.sh` | stdout |

The only host requirement is Docker. Builds go to `build/<target>/`, a ccache shared by every
worktree of a checkout makes rebuilds cheap.
Each image is tagged `jasperengine/<target>-build:<hash>`, the first 12 hex digits of a sha256 over
its Dockerfile and what that COPYs or ADDs from `docker/`; `:latest` names the newest build.
Worktrees with different Dockerfiles therefore keep one image each instead of retagging one name
back and forth (a rebuild on every switch, and an untagged image that `docker image prune` deletes
under a job still running it by id). The containers run the image by id.

### Unit tests: `tools/build.sh unit-tests`

Builds `jasperengine-core-tests` in the standalone engine tree (`build/linux/core-tests`, preset
`engine-linux-tests`, RelWithDebInfo) and `jasperengine-editor-tests` in the editor tree
(`build/linux/editor-tests`, preset `linux-ci`), runs both under Xvfb, and puts each result
through the gate (below). The test targets start their X server with `tools/with-xvfb.sh`, not
`xvfb-run`: xvfb-run waits forever when Xvfb signals ready before its `wait` starts (a loaded
builder), whereas the helper reads the display from `Xvfb -displayfd` within `XVFB_TIMEOUT`
seconds (30). An unfiltered run must execute at least 300 core and 150 editor tests,
with none skipped.

```sh
tools/build.sh unit-tests                                   # both binaries
UNIT_SUITE=core tools/build.sh unit-tests                   # one of them (core or editor)
UNIT_TEST_ARGS='--gtest_filter=Matrix4.*' tools/build.sh unit-tests
GRAPHIC_BACKEND=vulkan UNIT_SUITE=core tools/build.sh unit-tests   # GPU suites on lavapipe
WERROR=1 tools/build.sh unit-tests                          # what CI runs
RUN=0 tools/build.sh unit-tests                             # build only
```

The binaries also register with CTest (`ctest --preset linux-ci` in an IDE build), labelled
`unit`, `gpu` and `slow` by suite name.

Picking (S26): `JASPER_PICK_SIMD=scalar|avx2|neon` forces the mesh BVH's slab test path (CI runs
the picking suites a second time with `scalar`); `MeshPose.MatchesGpuSkinning` is a GPU suite (the
CPU pose against the skinned shaders, glcore and Vulkan). `JASPER_PICK_BUILD_DELAY_MS=<ms>` holds
every background mesh BVH build pending that long: the MCP tests use it to see the Scene view's
waiting click and refused drop (`tools/mcp/tests/test_scene_view*.py`).

### HTML UI, MCP, smoke, web

- `tools/build.sh htmlui-tests`: about 3,300 tests, 74 of them skipped on purpose (the gate allows
  exactly those). `HTMLUI_TEST_ARGS` filters, `JASPER_HTMLUI_PERF_SLACK` relaxes the performance
  targets on a loaded host (CI uses 3).
- The HTML UI rendering suites of S16 (`docs/specs/S16-ui-render-text.md`): `RenderBatch.*`,
  `RenderClip.*`, `RenderPrimitive.*`, `TextSharpness.*`, `TestFontEngine.AtlasBudget*` and, in
  Release builds, the benchmark `HtmlUIRenderPerf.*` (`fixtures/bench/`; results in
  `build/linux/htmlui-render-bench[-vulkan].json`, the table in `docs/html-ui/render-benchmark.md`;
  its timings are judged on an otherwise idle machine). Changed HTML UI goldens need a row in
  `docs/html-ui/goldens-S16.md`: `python3 tools/htmlui/check_golden_ledger.py` (tested by
  `tools/ci/test_check_golden_ledger.py`) fails on a changed golden without one and on any change
  of a Chromium reference (`golden/browser/**`).
- `tools/build.sh mcp-tests` runs `tools/mcp/run-tests.sh` in the Linux image against
  `build/linux/editor-install`. `MCP_SUITE=ci` (the default) runs the files that declare
  `CI = True`; `MCP_SUITE=all` runs every file and needs the default fixture project, which is
  not in git: `JASPER_FIXTURE`, else `build/fixture-unity` of the checkout or the main checkout,
  else `~/je/fixture-unity` (the build servers' copy), mounted at `/fixture`. The harness looks in
  the same places (`harness.default_fixture()`); without a fixture, the tests that need it skip
  and say where they looked. `MCP_SUITE=all` first builds the mesh-raycast web test project
  (`build/web-mesh-raycast`, the web image; up to date after the first time) for
  `test_mesh_raycast.py`'s web latency gate. The Linux image carries the suite's test tools
  (lua-language-server, clangd, FFmpeg with AV1, chrome-headless-shell), so none of their tests
  skips there. See `docs/mcp/README.md` for writing MCP tests.
- An MCP test asserts behaviour; a wall-clock budget (a CLI start in 300 ms, a reload frame under
  16 ms, ...) goes through `harness.over_budget(what, measures, budget)`, which prints a `[bench]`
  line and fails only when `JASPER_MCP_PERF_SLACK` is set: a shared build server's load alone
  misses those budgets. The benchmark run enforces them on the benchmark-only server E:
  `rb.sh E --img 'JASPER_MCP_PERF_SLACK=1 dbus-run-session -- tools/mcp/run-tests.sh
  tools/mcp/tests/test_call_cli.py tools/mcp/tests/test_hot_reload.py tools/mcp/tests/test_checks.py'`.
- `tools/smoke-test.sh <project>`: see its header. It also checks that the exported project
  configures as a user's game does: no presets file, no warnings as errors, no sanitizer.
- `tools/build.sh web-gate` serves `build/web` with cross-origin isolation, opens
  `jasperengine-project.html?jasperengine_ready_signal=1` in the web image's pinned Chrome for
  Testing headless shell, and fails on an uncaught exception, a console error, an engine error
  line, a missing ready line, or a centre pixel or `crossOriginIsolated` that differ from
  `tools/web/gates/sample-project.json`. `GATE=sample-project-threads` checks the pthreads build
  (`WEB_THREADS=1 tools/build.sh web`). `tools/web/serve.py` serves a build the same way for a
  look in a desktop browser.
- A gate baseline can also list `console` lines (`{"text", "level"}`, `"ordered": true`): each must
  appear at that console level (log, info, warning, error), in order; an error listed there is
  expected and fails nothing. `GATE=core-utilities` (after `WEB_PROJECT=core-utilities tools/build.sh
  web`) checks the logger's browser console sink and a Lua `Timer` that way.
- `GATE=mesh-raycast` (after `WEB_PROJECT=mesh-raycast tools/build.sh web`) times 10,000
  MeshRaycast rays against 1,000 meshes in WebAssembly and fails on a p99 over 4 ms (S26 AC17).
- The other test projects with a gate: `atmosphere` (S12), `post-fx` (S13) and `terrain`, a
  terrain displaced by a 16-bit heightmap, which WebGL2 and GLES3 cannot sample as R16 (the
  renderer uploads it as R32F or R16F there).

### Test projects

`tools/test-projects/<name>/` are small engine projects (`main.cpp`, `lua/`, `assets/`) that a
platform test builds instead of `engine/project`: `WEB_PROJECT=<name> tools/build.sh web`
(`build/web-<name>`, checked by `GATE=<name> tools/build.sh web-gate` with
`tools/web/gates/<name>.json`) and `ANDROID_PROJECT=<name> tools/build.sh android`
(`build/android-<name>/app-debug.apk`, checked by the emulator runner below). Keep one project per
behaviour under test, with its expectations next to it.

### Android emulator: `tools/android-emulator-test.sh`

Installs an APK on a headless Android emulator, launches it with its LAUNCHER intent and checks
its logcat. It needs KVM: run it on a Linux host with `/dev/kvm` (build servers A and C; not B).

```sh
ANDROID_PROJECT=core-utilities tools/build.sh android
tools/android-emulator-test.sh build/android-core-utilities/app-debug.apk --ordered \
    --expect-file tools/test-projects/core-utilities/android-expect.txt
tools/android-emulator-test.sh build/android/app-debug.apk --seconds 30 --forbid 'E/JasperEngine'
```

- The image is `docker/android-emulator.Dockerfile`: the Android build image plus the emulator and
  an x86_64 Automated Test Device system image (`EMULATOR_API` 34, `EMULATOR_TAG` aosp_atd, build
  arguments). The first run builds it (about 2 GB). The APK needs the x86_64 ABI, which
  `tools/build.sh android` builds with the others.
- Each run boots from wiped user data (`-no-window -no-snapshot -wipe-data`, `-gpu
  $EMULATOR_GPU`: see "The emulator's GPU" below),
  installs the APK with every permission granted, clears logcat, launches the app and polls
  logcat until every `--expect` line is there, the app crashes, or `--timeout` (120 s) passes.
- `--expect` matches a `logcat -v tag` line: `W/JasperEngine: [render] text` checks the priority,
  the tag and the text. `--ordered` requires the order given, `--forbid` fails on a line,
  `--expect-file` reads expectations one per line. A `FATAL EXCEPTION`, a native crash of the
  package or `Fatal signal` fails the run.
- Results in `--out` (default `build/android-emulator/`): `result.txt` (each expectation, found or
  missing, then PASS/FAIL), `logcat.txt` (`-v tag`), `logcat-full.txt` (`-v threadtime`),
  `screen.png` and `emulator.log`. Exit 0 pass, 1 fail, 2 could not run (no KVM, no boot, the
  install failed).
- The AVD lives on a tmpfs inside the container (`EMULATOR_TMPFS_GB`, default 10; only the pages
  the run writes use memory), so a run leaves nothing on the host disk but `--out`. A boot takes
  about 40 s on servers A and C; a whole run about 70 s.
- `EMULATOR_BOOT_TIMEOUT`, `EMULATOR_MEMORY`, `EMULATOR_CORES` and `JASPER_DOCKER_IMAGE` (a
  prebuilt emulator image) tune it. `EMULATOR_GPU` is passed to `-gpu` (below).
- Every run also fails on what the GLES path logs when it goes wrong, expected or not: an
  `OpenGL error` (NativeEngine reads `glGetError` after every frame; a debug build's sokol also
  asserts on one after each GL call, which the crash check catches), a shader that did not compile
  or link (sokol's GL backend, the runtime shader compiler), an incomplete framebuffer, an EGL
  error or a failed swap, a sokol panic, and libEGL's "call to OpenGL ES API with no current
  context".

#### Rendering gate: `--scene` and `--golden`

The logcat says the engine started; only the screen says it drew. `--golden DIR` makes each run
wait for `I/JasperEngine: Engine presented: 30 frames since ready` (NativeEngine reports every
successful swap, `Engine::systemFramePresented`, and the engine counts them from its ready line),
then screenshots the device (`adb exec-out screencap`) and checks it with
`tools/android-screen-check.py`:

- **content**: a screen where one colour (within 8 per channel) covers all but 2 % of the pixels
  fails as BLANK: a black screen, or a frame that is only the clear colour;
- **golden**: the screenshot, box-filtered to 270 pixels wide (270 x 600 for the emulator's
  Pixel 6, 1080 x 2400), is compared with `DIR/<scene>.png` by `tools/mcp/pngdiff.py`: at most 0.25 %
  of the pixels (405) may differ by more than 24 in a channel (the gate's missing caption is
  1.3 %; two runs, and the two GPU modes, differ by none). `<scene>.scaled.png` and
  `<scene>.diff.png` (differing pixels in red) are written next to the screenshot.
  `JASPER_UPDATE_GOLDEN=1` writes the goldens instead (the content check still applies); look at
  them before committing.

`--scene NAME` (repeatable) launches the app once per scene with the property
`debug.jasperengine.scene` set to it (a test project reads it, `JASPERENGINE_SCENE` elsewhere),
each run checked on its own in `--out/<scene>/`; `{scene}` in an expectation is the run's scene.
The gate's project is `tools/test-projects/render-gate`: a lit 3D scene (`lit3d`), 2D and UI
(`ui2d`: a textured Image, Polygons, a Text) and an HTML UI document (`htmlui`), goldens in its
`golden/`:

```sh
ANDROID_PROJECT=render-gate ANDROID_ABIS=x86_64 tools/build.sh android      # server B or D
tools/android-emulator-test.sh build/android-render-gate/app-debug.apk \
    --expect-file tools/test-projects/render-gate/android-expect.txt \
    --scene lit3d --scene ui2d --scene htmlui --golden tools/test-projects/render-gate/golden
```

`ANDROID_ABIS=x86_64` builds only the emulator's ABI (a quarter of the build).

#### The emulator's GPU

The emulator renders on the CPU. `EMULATOR_GPU` defaults to `swangle_indirect`: ANGLE on
SwiftShader's Vulkan, GLES 3.1, the path Google maintains; the goldens are made there.
`EMULATOR_GPU=swiftshader_indirect` runs the emulator's older GLES 3.0 translator on legacy
SwiftShader (the runner's only mode before BF3); the render gate passes there too.

That translator is why Android "never rendered" in the emulator (BF3). It decides texture
completeness without `GL_TEXTURE_IMMUTABLE_LEVELS`: a texture made with `glTexStorage2D(levels =
1)` and sampled with a mipmapped min filter (sokol's samplers always use one) counts as
incomplete unless its `GL_TEXTURE_MAX_LEVEL` is 0, and samples (0, 0, 0, 0). Mip-chained textures
(an Image's) drew; every one-level texture did not: the text font atlas, every HTML UI layer and
post-processing input, so a scene showed its clear colour (or black) with untextured geometry
and Images over it. No GL error, no shader log: per-draw readbacks found the font atlas full and
the draw writing alpha 0. sokol now states the levels as the max level
(`_sg_gl_texstorage`, `SokolGlTextureLevels` in the core tests), which conformant GL ignores. A
real device is affected only if its driver has the same completeness bug; the emulator gate
checks both modes. Neither mode can show what depends on a real GPU: driver shader compilers and
precision, tiler bandwidth, ASTC/ETC2 hardware decode (the emulator decodes in software), and
frame timing (SwiftShader draws a frame in tens to hundreds of milliseconds).

### Native crash diagnostics

`jasperengine-crash-test` is built with core tests. `ctest --test-dir build/linux/core-tests
-R 'native-crash|posix-crash' --output-on-failure` runs each deliberate fatal mode in a child,
checks registers, frames, log tail, signal chaining and overflow reporting, and checks the POSIX
object for allocation/stdio/mutex imports. It also verifies a main-thread hang frame. These
checks work in the ASan/UBSan tree too (`core-tests-address-undefined`). Windows/macOS desktop
CTest builds register the same child-process checks; Windows additionally requires a minidump.

For Android (server A or C), build `ANDROID_PROJECT=crash-diagnostics tools/build.sh android`,
then `tools/android-emulator-test.sh build/android-crash-diagnostics/app-debug.apk
--expect-native-crash --expect 'JASPERENGINE CRASH:'`. This fixture segfaults at frame 10.
The opt-in flag requires the engine report **and** a matching debuggerd tombstone, saves both
under the emulator result directory, and accepts that deliberate crash. Ordinary emulator tests
continue to fail on any native crash.

## Where a test goes

| Code under test | Test file | Target |
|---|---|---|
| `engine/core/<area>/X.cpp` | `engine/tests/core/<area>/XTest.cpp` | `jasperengine-core-tests` |
| `engine/renders/...` | `engine/tests/core/render/...Test.cpp` | `jasperengine-core-tests` |
| `editor/<area>/X.cpp` | `editor/tests/<area>/XTest.cpp` | `jasperengine-editor-tests` |
| the HTML UI | `engine/tests/htmlui/` | `jasperengine-htmlui-tests` |
| `engine/core/<area>/X.cpp` that a test drives through editor scenes (the editor's static `App`, physics worlds, model loads) | `editor/tests/engine/<area>/XTest.cpp` | `jasperengine-editor-tests` |
| an MCP tool or agent-facing behaviour | `tools/mcp/tests/test_<area>.py`, with `CI = True` | MCP suite |

Both unit-test targets glob `*Test.cpp` under their directory, so a new file needs no CMake
change: add it and rebuild. Test data goes in `engine/tests/core/fixtures/` or
`editor/tests/fixtures/` (the asset root of `TestSystem` for that target), golden images in
`engine/tests/core/golden/`. `engine/tests` is excluded from the engine SDK copy and
`editor/tests` is never installed.

### Naming

- Files are `<Subject>Test.cpp`. Suites are `<Subject>`, tests `TEST(Subject, VerbPhrase)` or
  `TEST_F(SubjectTest, VerbPhrase)` in CamelCase: `TEST(Quaternion, SlerpReturnsTheEndPoints)`.
- A suite that needs a GPU ends in `Gpu` (`RenderGpu`) and starts each test with
  `JASPER_REQUIRE_GPU()`: without a device it fails, it never skips. GPU suites run on Linux;
  Windows and macOS CI run the CPU suites with `--gtest_filter=-*Gpu*`.
- A suite whose tests take more than 5 s each ends in `Slow`. Any other test stays under 1 s, and
  both binaries' CPU suites together under 60 s on 4 cores.
- CTest labels follow the names: `gpu`, `slow`, else `unit`.

### What the support library gives a test

`engine/tests/support/` is `jasperengine-test-support`, linked into every test target, with a
shared `main()` (`TestMain.cpp`) that initialises the engine without a GPU:

- `TestSystem::get()`: the engine's `System` for tests; every log line is recorded.
- `LogCapture` and `EXPECT_NO_ENGINE_ERRORS()`: assert on what the engine logged since the test
  (or the capture) started: `LogCapture log; ...; EXPECT_EQ(log.count(D_LOG_WARN, "missing"), 1u);`.
- `TempDir`: a fresh directory per test (`<build>/test-tmp/<Suite>.<Test>-...`), removed at the
  end of its scope; `JASPER_TEST_KEEP_TEMP=1` keeps them.
- `GpuEnvironment::ensure()` / `JASPER_REQUIRE_GPU()`: the headless device (Xvfb with GLX, or
  lavapipe for Vulkan), frames rendered into `GpuEnvironment::target()`.
- `ImageCompare`: `EXPECT_GOLDEN("name")` compares the target with `golden/name.png`; on a
  mismatch `<build>/test-output/` gets `name.actual.png` and `name.diff.png`.
- googletest 1.17 and googlemock (`libs/googletest/`, tests only).
- `engine/tests/core/support/AllocationCounter.h` (core tests and the S06 benches): counts heap
  allocations at `malloc` (wrapped by GNU ld, so on Linux; elsewhere `countsAllocations()` is
  false and the counts stay 0). `AllocationCountScope` counts the whole process,
  `ThreadAllocationCount` only the calling thread (for code that must not allocate while the
  logger's drain thread or the workers may); `perfSlack()` is the factor wall-clock budgets are
  enforced with: `JASPER_TEST_PERF_SLACK`, unset (0) by default and under a sanitizer, where the
  budgets are only printed. `*Bench` suites (`LoggerBench`, `SchedulerBench`, `SignalBench`, S10's
  `ShadowBench`), `SchedulerTest.Process10kUnder2ms` and S12's `AtmosphereNoise` bake budgets check
  their budgets that way and print `[bench]` lines; the
  editor suite's `editor::test::perfSlack()` does the same for `CommandHistoryBench` (S24),
  `AutosaveEncodeBench` (S03), `VcsBench` (S29: status poll, snapshot swap, scene diff; the
  main-thread encode is reported) and `ReflectCodecBench`/`ReflectResolveBench` (S08; its MCP R19 frame
  cost, `test_reflection.py::test_inspector_frame_cost`, reads the same variable) (ratios such as a report's overhead stay checked everywhere). The MCP
  texture storm's frame budget (`test_jobs_memory_ui.py`, S06 AC23) reads the same variable; it assumes
  a machine whose decode workers and software renderer do not share cores (8+ hardware threads):
  E's 4 vCPU measure a storm p95 of 1.7x the idle frame against the 1.5x budget, so it runs there
  only with a slack above that. A
  shared build server's load alone misses them (10k timers in 20 ms instead of 2), so an ordinary
  unit test run never fails on wall-clock time; the benchmark run enforces them on the
  benchmark-only server E, which runs one job at a time:

  ```bash
  rb.sh E env JASPER_TEST_PERF_SLACK=1 \
      UNIT_TEST_ARGS='--gtest_filter=*Bench*:SchedulerTest.Process10kUnder2ms:AtmosphereNoise.*' tools/build.sh unit-tests
  ```

### Golden images

`JASPER_TEST_UPDATE_GOLDEN=1` rewrites the goldens a run compares (the HTML UI suite also takes
`JASPER_HTMLUI_UPDATE_GOLDEN=1`); goldens are only written on glcore, and Vulkan has to match
them. The goldens are renders of Mesa's llvmpipe (glcore) and lavapipe (Vulkan), the test image's
devices (`GpuEnvironment::isReferenceRenderer`); a comparison a hardware GPU draws differently gets
its looser limits from `test::hardwareTolerance(hardware)` (a browser-parity fixture from
`tolerance.hardware`), measured on that GPU, while the reference renderers keep the strict one. Re-baseline one suite at a time, look at every changed image before committing it, and say
in the commit why it changed:

```sh
JASPER_TEST_UPDATE_GOLDEN=1 UNIT_SUITE=core UNIT_TEST_ARGS='--gtest_filter=RenderGpu.*' tools/build.sh unit-tests
GRAPHIC_BACKEND=vulkan UNIT_SUITE=core UNIT_TEST_ARGS='--gtest_filter=RenderGpu.*' tools/build.sh unit-tests
```

The cooked fixtures of the texture format test (`engine/tests/core/fixtures/texture_formats/`, S14
AC8: every `GpuTextureFormat` as a cooked 32 px KTX2 with the CPU decode of each level, which
`TextureFormatsGpu` compares the GPU's sampling with) come from the editor's encoders and
decoders. `JASPER_TEST_UPDATE_FIXTURES=1` rewrites them; otherwise `TextureFormatFixtures` checks
that they are complete and that the editor still decodes them the same:

```sh
JASPER_TEST_UPDATE_FIXTURES=1 UNIT_SUITE=editor UNIT_TEST_ARGS='--gtest_filter=TextureFormatFixtures.*' tools/build.sh unit-tests
```

The cooked families of `tools/test-projects/texture-formats` (the web gate and the Android
emulator run) are rewritten with `tools/test-projects/texture-formats/make_assets.sh` after
`tools/build.sh linux`.

The post FX goldens (`tools/mcp/tests/test_post_fx_golden.py`, S13) live in
`tools/mcp/tests/golden/post_fx/` and are rewritten with `JASPER_POST_FX_UPDATE_GOLDEN=1`, except
`defaults.png`: the post-processing fixture with every field at its default, captured by that test
on the editor before S13 (oe-port `8d78ae763`, after S10 and S12), which the S13 editor must match exactly. Re-capture
it only from an editor without S13's changes.

## No silent green

- `tools/ci/gtest-gate.py` reads the googletest XML of every run and fails when the file is
  missing (the binary crashed), when no test ran (a filter that matches nothing is not a pass),
  when fewer tests ran than expected, when a test failed, or when more tests were skipped than
  allowed (0, except the HTML UI suite's recorded 74). It lists every disabled test. Every CI
  test step goes through it; `tools/ci/test_gtest_gate.py` tests it.
- Declared tests are the tests that ran plus the disabled ones. A test that flakes twice in a week
  is quarantined with a `DISABLED_` prefix and a tracking issue; the gate's list keeps it visible.
  Opt-in tests that need what the repository does not hold (`VatImportSideFx.*`: the SideFX VAT 3
  tutorial exports, `JASPER_VAT_SIDEFX_EXAMPLES`) or are measurements to run by hand
  (`EntityDestroyScaling.Profile`, `JASPER_DESTROY_PROFILE`) are `DISABLED_` too, never a skip:
  `--gtest_also_run_disabled_tests` with the filter runs them.
- A behavioural claim needs a red control: revert the behaviour locally, show the test failing
  (paste the output into the pull request), restore it. A bug fix shows the test red before the
  fix and green after.
- Keep the XML: CI uploads `build/test-results/` from every job.
- The first tests of an area are characterization tests: they pin today's behaviour. A change
  that alters that behaviour on purpose updates those tests in the same commit and says so.

## How a spec adds tests

1. Put unit tests where the table above says, named as above. Use `TempDir` for files and
   `EXPECT_NO_ENGINE_ERRORS()` where the engine logs.
2. Rendering changes add a `*Gpu` test with a golden in `engine/tests/core/golden/`, created on
   glcore and matched on Vulkan.
3. Agent-facing features add MCP tests, CI-capable (`CI = True`, own project).
4. Run `tools/build.sh unit-tests` (and the suites of the touched area) on a build server; record
   the red control.
5. Code must build warning-free with `WERROR=1` (below).

## Build options and presets

`CMakePresets.json` (editor tree) and `engine/CMakePresets.json` (standalone engine tree, never
copied into the editor install or an export) hold the standard configurations:

| Tree | Presets |
|---|---|
| editor | `linux-release`, `linux-debug`, `linux-vulkan-release`, `linux-asan`, `linux-tsan`, `linux-unity`, `linux-ci`, `windows-msvc-release`, `windows-msvc-debug`, `windows-msvc-asan`, `windows-vs2022`, `macos-release`, `macos-universal-release`, `macos-asan` |
| engine | `engine-linux-release`, `engine-linux-debug`, `engine-linux-tests`, `engine-linux-tests-vulkan`, `engine-linux-asan`, `engine-linux-tsan`, `engine-windows-msvc-tests`, `engine-macos-tests`, `engine-web-debug`, `engine-web-release`, `engine-web-threads` |

```sh
cmake --preset linux-ci && cmake --build --preset linux-ci && ctest --preset linux-ci
cmake -S engine --preset engine-linux-tests && cmake --build --preset engine-linux-tests   # (from engine/)
```

Presets build into `build/presets/<name>`; `tools/build.sh` uses them with its historical
directories (`-B build/linux/editor`, ...) and adds the ccache launcher, which presets never set.
The Windows MSVC presets need the developer environment (`vcvars64`); Android stays on Gradle and
iOS on the Xcode workspace. `tools/ci/check-presets.sh` configures every Linux preset and checks
the option errors.

| Option | Default | Effect |
|---|---|---|
| `JASPERENGINE_TESTS` | OFF | the tree's unit-test target (`jasperengine-core-tests` in `engine/`, `jasperengine-editor-tests` in the editor tree) |
| `JASPERENGINE_SANITIZE` | empty | `address;undefined` or `thread` on every target of the tree (below) |
| `JASPERENGINE_WARNINGS` | ON (editor tree), OFF (`engine/`) | the warning set on our own targets |
| `JASPERENGINE_WERROR` | OFF | warnings of our targets are errors (needs `JASPERENGINE_WARNINGS`) |
| `JASPERENGINE_UNITY_BUILD` | OFF | unity builds of our own targets, 16 sources per unit |

`engine/CMakeLists.txt` is also the top-level file of every exported game, so all of these default
to off there, and the exporter never sets them: a game configures and builds exactly as before.

### Sanitizers

```sh
SANITIZE=address,undefined tools/build.sh unit-tests   # build/linux/core-tests-address-undefined, Debug
SANITIZE=thread UNIT_TEST_ARGS='--gtest_filter=-*Gpu*' tools/build.sh unit-tests
SANITIZE=address,undefined tools/build.sh linux        # an instrumented editor: build/linux/editor-address-undefined
SANITIZE=address,undefined HTMLUI_TEST_ARGS='--gtest_filter=-*Golden*' tools/build.sh htmlui-tests
```

`engine/cmake/JasperSanitizers.cmake` adds `-fsanitize=<list> -fno-omit-frame-pointer` to every
target of the tree, vendored libraries included (uninstrumented code gives TSan false positives),
plus `-fno-sanitize-recover=undefined -fno-sanitize=vptr` with `undefined` (`function` too on
Clang), and defines `JASPERENGINE_SANITIZE_ADDRESS`, `_UNDEFINED` and `_THREAD` so tests can relax
time budgets. `thread` with `address` is a configure error, as are `thread` and `undefined` on MSVC
and any sanitizer on Emscripten, Android or iOS. On MSVC, `address` switches to the DLL runtime,
drops `/RTC1`, links with `/INCREMENTAL:NO`, defines `_DISABLE_STL_ANNOTATION`, and copies
`clang_rt.asan_dynamic-x86_64.dll` next to every executable so they start from a plain shell.

`tools/build.sh` passes these runtime options on every run:
`ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:strict_string_checks=1`,
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:suppressions=tools/sanitizers/ubsan.supp`,
`LSAN_OPTIONS=suppressions=tools/sanitizers/lsan.supp`,
`TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=tools/sanitizers/tsan.supp`.
The suppression files list third-party code only (Mesa, X11, fontconfig, vendored libraries),
each entry with its reason. Engine and editor code is never suppressed: fix it.

### Warnings

`jasperengine_target_warnings()` (`engine/cmake/JasperWarnings.cmake`) puts
`-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare`
(MSVC: `/W3 /wd4244 /wd4267 /wd4305`) on `jasperengine`, the editor executables, the project
executable and the test targets; `JASPERENGINE_WERROR` adds `-Werror -Wno-error=deprecated-declarations`
(MSVC `/WX`). Vendored libraries keep `-w`/`/w` and `SYSTEM` includes. CI writes each job's warning
count per target to its summary (`tools/ci/warning-summary.py`).

Werror is enforced with GCC in the `unit` and `e2e` jobs (`WERROR=1`). It is never enforced on
Emscripten, the Android NDK, the Xcode workspace or exported projects: those compilers move under
us and compile user code. Code from any stream must build warning-free with GCC.

**The ratchet.** These groups are off today; turning one on is a change of its own, with its
cleanup: `-Wsign-compare` (GCC/Clang), and MSVC's `C4244`, `C4267` and `C4305` (conversions
that lose data). `-Wunused-parameter` and `-Wmissing-field-initializers` stay off by design.

### Unity build

`JASPERENGINE_UNITY_BUILD=ON` (`UNITY=1 tools/build.sh linux`, preset `linux-unity`) merges the
sources of our own targets 16 at a time; Objective-C++ and generated sources are always compiled
alone, and a source that cannot share a unit is fixed (a colliding file-local helper gets a
name of its own) or listed in `jasperengine_skip_unity_build(REASON ... SOURCES ...)`: today the
sources that include Xlib (macros such as `None` and `Status`), and the HTML UI and MCP tool
sources, which keep one generic helper set (`lower`, `trim`, `Target`...) per file. A new
`namespace {}` helper in our code must not reuse a name another source of the target defines at
file scope. The nightly workflow builds it.

## CI

`.github/workflows/ci.yml` runs on every push and pull request:

| Job | What | Budget |
|---|---|---|
| `unit` | notices, the CI scripts' tests, the data-safety lints (`tools/check_raw_writes.py`, `tools/check_stream_keys.py`), presets, `WERROR=1 tools/build.sh unit-tests` | 25 min |
| `sanitizers` | `SANITIZE=address,undefined tools/build.sh unit-tests`, GPU suites included | 35 min |
| `htmlui` | `tools/build.sh htmlui-tests` (OpenGL) | 45 min |
| `e2e` | `WERROR=1 tools/build.sh linux`, the install's notices, `htmlui-editor-tests`, `MCP_SUITE=ci tools/build.sh mcp-tests -j 2`, the post-processing smoke test | 60 min |
| `web` | web presets, `tools/build.sh web`, `tools/build.sh web-gate` | 30 min |

Each job builds its Docker image with the Actions layer cache and passes it to `tools/build.sh`
as `JASPER_DOCKER_IMAGE`, so CI and a build server run the same image the same way; reproduce a
red job with the command in its step. The Linux packaging job keeps its bare `ubuntu:22.04`
container (the AppImage's glibc floor). Windows and macOS (on demand, pull requests, tags) build
with `JASPERENGINE_TESTS=ON` and run both unit binaries without their GPU suites through the gate.
The release job checks the tag first (`tools/ci/check-release-tag.sh`: `vX.Y[.Z]`, and exactly
the built commit; `git describe` is the only version source, there is no VERSION file).

`.github/workflows/nightly.yml` (daily on `main` and `oe-port`, or on demand): the HTML UI suite on
Vulkan and under ASan + UBSan, the unit tests under TSan, the unity builds, the smoke test of every
fixture project on OpenGL plus the GLES3 and Vulkan sets (`tools/ci/nightly-smoke.sh`), and the
threaded web build through the gate, with and without cross-origin isolation. A failure opens or
comments on the open issue labelled `nightly`.

Third-party notices: `tools/ci/check-notices.py` fails when a directory under `libs/`,
`engine/libs/` or `engine/libs/shadercompiler/libs/` lacks a `NOTICE.md` row or a licence file;
the editor install carries `NOTICE.md`, `LICENSE` and `licenses/<library>-LICENSE`.

The Chrome for Testing headless shell in `docker/web.Dockerfile` and `uv` in
`docker/linux.Dockerfile` are pinned by version and sha256 and bumped by hand, as are the MCP
harness's packages (`tools/mcp/requirements.txt`).

## Record (S02)

Measured on build server B (Docker, GCC 11.4) while S02 landed. Later specs append their own
records to their spec files, not here.

### First batch counts (AC8)

From the run's googletest output; a suite is a googletest test suite.

| Area | Suites | Tests | Minimum |
|---|---|---|---|
| ECS + registry | `ComponentArray`, `EntityManager`, `ComponentManager`, `Signature`, `EntityRegistryTest`, `TagManagerTest`, `LayerCollisionMatrix`, `Physics` | 95 | 60 |
| Math | `Vector2/3/4`, `Quaternion`, `Matrix3`, `Matrix4`, `AABB`, `OBB`, `Ray`, `Plane`, `Sphere`, `Rect` | 179 | 80 |
| Stream | `StreamEnum` (every enum pair), `StreamComponent` (every component type), `StreamValue`, `StreamScene`, `Fixtures/StreamFixtureScene` (17 scenes) | 140 | 100 |
| CommandHistory | `CommandHistoryTest` | 42 | 25 |
| AssetDatabase | `AssetDatabaseTest`, `AssetDatabaseGuid` | 36 | 25 |
| Renderer CPU | `PipelineState`, `ShaderManifest`, `ShaderReference`, `ShaderDataSerializer`, `ShaderKey`, `ShaderVariantCollection` | 51 | 30 |
| `RenderGpu` | `render_unlit_plane`, `render_lit_cube` goldens | 2, glcore and Vulkan (lavapipe) | 2 |

Totals: `jasperengine-core-tests` 356 (utilities included), `jasperengine-editor-tests` 218.

### Red controls (AC9)

Each behaviour was reverted in a scratch worktree, `tools/build.sh unit-tests` run, and restored:

| Area | Reverted | Failing test (first assertion) |
|---|---|---|
| Math | `Matrix4::transpose` copies instead of swapping | `Matrix4.TransposeSwapsRowsAndColumns`: `t.get(col, row)` 1, `m.get(row, col)` 0 |
| ECS | `EntityRegistry::destroyEntity` skips `componentManager.entityDestroyed` | `EntityRegistryTest.DestroyEntityDropsEveryComponent`: `hasEntity(entity)` true |
| Renderer | `ShaderManifest` keeps colour defaults in sRGB | `ShaderManifest.ColourDefaultsAreStoredLinear`: 0.5 vs `pow(0.5, 2.2)` |
| Stream | `lightTypeToString(SPOT)` writes `"point"` | `StreamEnum.LightTypeRoundTrips`: 1 vs 2 for `'point'` |
| CommandHistory | `canRedo()` is `index <= list.size()` | `CommandHistoryTest.StartsWithNothingToUndoOrRedo` and 6 more |
| AssetDatabase | `refresh()` never finds the moved file's old entry | `AssetDatabaseTest.RefreshSeesMovedFiles`: 2 changes, expected 1 |

The run: core 356 ran, 3 failed; editor 218 ran, 9 failed; `gtest-gate` exit 1 for both.

Sanitizer red controls (a scratch test, not committed): a read of a deleted `std::vector`'s
buffer under `SANITIZE=address,undefined` stops the run with `ERROR: AddressSanitizer:
heap-use-after-free ... SanitizerRedControlTest.cpp:10`, and two threads incrementing one `int`
under `SANITIZE=thread` with `WARNING: ThreadSanitizer: data race ... SanitizerRedControlTest.cpp:17`;
in both runs the binary aborts and `gtest-gate` fails on the missing XML.

### Sanitizer and warning runs

| Run | Result |
|---|---|
| `SANITIZE=address,undefined tools/build.sh unit-tests` | core 356 ran (GPU suites included), editor 218 ran, 0 failed, no leak report |
| `SANITIZE=thread UNIT_SUITE=core UNIT_TEST_ARGS='--gtest_filter=-*Gpu*' tools/build.sh unit-tests` | 354 ran, 0 failed, no race report |
| `WERROR=1 tools/build.sh unit-tests` | both trees build with `-Werror`; 356 + 218 ran, 0 failed |

What the first ASan runs found, and the fixes: ThorVG replaced the global `operator new`/`delete`
but not the sized `delete`, so ASan's sized `delete` freed ThorVG's `malloc` blocks
(alloc-dealloc-mismatch in the first static initializer); the vendored ThorVG no longer replaces
them at all (`engine/libs/thorvg/CMakeLists.txt` lists the local patch), and the core tests'
allocation counter replaces the plain forms in the test binary only. The editor leaked the default sky's six decoded
faces (about 7 MB) every time a scene with `useDefaultTexture` was decoded after the first
(`ProjectUtils::setDefaultSkyTexture` now reuses the pooled faces). A `CameraComponent` value owns
a `Framebuffer` that only `RenderSystem` frees, when its entity is destroyed; the tests free the
ones they make. The headless GL context stays open until exit under ASan, or Mesa's driver
allocations show up as `<unknown module>` leaks. Later (BF5), about 1 MB of V-HACD hulls leaked
whenever Physics Placement's convex decomposition was cancelled while V-HACD handed its merged
hulls over: its finalize loop stopped and cleared the map that owned the rest. The vendored
header now frees them (`engine/libs/vhacd/README.jasper.md`);
`ConvexDecomposition.CancelWhileFinalizingFreesTheHulls` cancels at that point and measures the
heap. With it the editor tree under ASan + UBSan ran 871 tests with no leak report. A loaded
`Project` frees none of its scenes itself (the editor's lives as long as the process, the CLI's
have no renderer): an editor test that loads one closes it with `editor::test::closeProject`
(`editor/tests/support/EditorTestSupport.h`) before dropping it, or every scene and its render
leaks (S08's reflection tests leaked about 84 MB that way).

### Unity build (R25)

`UNITY=1 tools/build.sh linux` and the unity engine and test trees build. A cold build of
`jasperengine-editor` (the editor and the shared engine) in the Linux image, `-j 8`, no ccache,
Release, before the S01 merge: 1,361 s without unity, 1,034 s with it, **24 % faster**, under R25's
25 %. The option stays, the nightly workflow keeps building it (so new sources stay unity-safe),
and no preset or job recommends it for speed. The server ran other builds at the same time, so
the figure is approximate; re-measure on an idle host before deciding otherwise.

### MCP, smoke (after the S01 merge)

- `dbus-run-session -- tools/mcp/run-tests.sh tools/mcp/tests/test_protocol.py` in the Linux
  image: 12 passed.
- `MCP_SUITE=ci tools/build.sh mcp-tests -j 2` (AC13): 24 files, 121 passed, 0 failed, 1 skipped,
  346 s.
- `tools/smoke-test.sh tools/fixtures/post-processing` (AC14): PASS in 227 s, with the AC6 check
  of the exported tree (no presets file, no warning or sanitizer options in its cache).
- `WERROR=1 tools/build.sh linux`: 0 warnings on our targets.

### Web gate, Android, presets (after the S01 merge)

- `tools/build.sh web` then `tools/build.sh web-gate`: PASS (0 errors, ready line, centre pixel
  (255, 254, 254), `crossOriginIsolated` true). With a planted `Log::error` in
  `engine/project/main.cpp` the gate fails (red control, before the merge).
- `WEB_THREADS=1 tools/build.sh web`, `GATE=sample-project-threads tools/build.sh web-gate`:
  **fails, on a real engine problem**: 19 console errors "Tried to spawn a new thread, but the
  thread pool is exhausted". `engine/CMakeLists.txt` links threaded builds with
  `PTHREAD_POOL_SIZE=4`, while `Engine::systemInit` starts `hardware_concurrency() - 1` pool
  threads (plus Jolt's). The page still reaches the ready line and renders. The nightly
  `web-threads` job stays red until the pool size follows the engine's thread count. Served without
  COOP/COEP (`GATE_ARGS=--no-isolation`) the gate fails as it should (no isolation, no ready line).
- `tools/build.sh android`: builds.
- `tools/ci/check-presets.sh linux` and `web` (in their images), `check-notices.py`,
  `test_gtest_gate.py` (14), `test_check_notices.py` (10), `check-release-tag.sh` cases
  (`v1.2.3` at the commit passes; `v1.2.3-bad`, another commit, `v1.2` fail), actionlint 1.7.12
  on both workflows: pass.
- `tools/build.sh htmlui-tests` (AC11, on `libs/googletest` and `engine/tests/support`): 3,237 ran,
  74 skipped (3,311 declared), 0 failed, no golden changed. `tools/build.sh htmlui-editor-tests`:
  72 ran, 1 skipped (the live Google Fonts test), 73 declared.

### After the S03 merge

S03's tests (`engine/tests/core/io`, `editor/tests/data_safety`) run in the same binaries:
`jasperengine-core-tests` 381, `jasperengine-editor-tests` 247.

- `WERROR=1 tools/build.sh unit-tests`: 381 + 247 ran, 0 failed.
- `SANITIZE=address,undefined tools/build.sh unit-tests`: 381 + 247 ran, 0 failed, no leak report.
- `WERROR=1 tools/build.sh linux`, `tools/build.sh web` + `web-gate` (PASS), `tools/build.sh android`: pass.
- `test_protocol.py` and `test_data_safety.py`: 15 passed. `MCP_SUITE=ci tools/build.sh mcp-tests -j 2`:
  124 passed, 0 failed, 1 skipped. `tools/smoke-test.sh tools/fixtures/post-processing`: PASS.
- The unit job's lints: `check_raw_writes.py --self-test` and the tree pass, `check_stream_keys.py`
  (58 tables) passes; notices, presets and actionlint pass.

### S16 (HTML UI rendering cost and text)

Round 2, on oe-port 50c7afd3d (S11, S17 merged; server B with `JASPER_HTMLUI_PERF_SLACK=3`, timings
on the benchmark host E):

- `tools/build.sh htmlui-tests` (glcore, without the perf suites and `EffectLifecycleTest.Hundred*`):
  3,379 ran, 0 failed, 74 skipped. `BrowserParity` (the 277 Chromium references, unchanged) and the
  batching equivalence pass.
- `HTMLUI_BACKEND=vulkan`, the whole suite twice: 3,379 ran, 0 failed each. The earlier
  order-dependent `RenderPrimitive.MatchesTheMeshPath` failures were 8-sample layer targets on
  lavapipe (it has 1 and 4): invalid Vulkan usage that drew into foreign memory; HTML UI targets now
  take the device's highest count up to the asked one, and the comparison skips 8 there.
- `JASPER_HTMLUI_S16_OFF=1` against the pre-S16 goldens (AC1): see the spec's Deviations
  (`p1/texture_effects` traced and fixed).
- `HTMLUI_TEST_ARGS=--gtest_filter=HtmlUIRenderPerf.*` on the benchmark host: 9 passed
  (`docs/html-ui/render-benchmark.md`; B2 44 % of the baseline engine time, B6 43 %).
- Gate on 592d8ed28 (B): `WERROR=1 tools/build.sh linux`, `tools/build.sh htmlui-editor-tests`,
  `tools/build.sh unit-tests` (core and editor), `tools/htmlui/run-rmlui-tests.sh --docker`,
  `tools/build.sh web` + `web-gate`, `tools/build.sh android`, `tools/smoke-test.sh
  tools/fixtures/post-processing` (glcore and Vulkan): pass. `docs/mcp/TOOLS.md` regenerated with
  `gen_tool_docs.py` (195 tools).
- MCP (`test_html_ui_*.py`, `test_ui_render_stats.py`, `test_protocol.py`, `test_profiler.py`,
  `test_tool_budget.py`): S16's failures fixed (`inspect_ui` over its 1792-char budget; the render
  stats test counting the cards the list's scrollport culls; S17's example play goldens
  `inventory`/`server_browser` predating S16, updated with ledger rows). The others failing on this
  oe-port (`test_html_ui_component` S04 asset type, `test_html_ui_phase2`/`_integration` S17 patch
  reload, `test_html_ui_effects` "Loading" dialog under load, `test_html_ui_export`,
  `test_html_ui_agent_sim`) are fixed on BF2's branch, whose run on oe-port passes them.

Final tree (oe-port 64d735104, S20 and S28, merged; f6c0639f0 and the docs after it):

- `tools/build.sh htmlui-tests` (glcore): 3,480 ran, 0 failed, 75 skipped. `JASPER_HTMLUI_S16_OFF=1`
  against the pre-S16 goldens (AC1): 3,480 ran, 0 failed. `HTMLUI_BACKEND=vulkan`, twice: 3,480
  ran, 0 failed each (server B).
- `HtmlUIRenderPerf.*` on the benchmark host E: 9 passed (B2 41 % of the baseline engine time).
- Gate on C: `WERROR=1 tools/build.sh linux`; `gen_tool_docs.py` (225 tools, no change); MCP
  (`test_html_ui_*.py`, `test_ui_render_stats.py`, `test_protocol.py`, `test_profiler.py`,
  `test_tool_budget.py`, `test_tool_conventions.py`, `test_search_organisation.py`): 345 passed,
  3 failed, the three failing without S16 too: `test_html_ui_checks::test_cli_default_includes_ui`
  (S14's `texture_imports` in the check summary; the test is updated on the origin/main sync
  branch) and `test_html_ui_video_export`'s two export tests (the exported `poster.png`, S14).
- On 14dcdfed6 (the S20 merge, C): `tools/build.sh unit-tests` core 1,331 and editor 881 ran,
  0 failed; `htmlui-editor-tests` 76 ran, 0 failed; `run-rmlui-tests.sh --docker` 37,670
  assertions pass; web + web-gate and android pass.
- Smoke on a build directory made before S20: the export copied the old
  `core/action/ParticleSystem.*` that `build/linux/editor-install/bin/engine` still held (S20
  moved them to `core/object`), and the exported build failed; removing those installed engine
  copies before the build fixes it: `tools/smoke-test.sh tools/fixtures/post-processing` (glcore
  and Vulkan), web + web-gate pass on the final tree.

Landing gate (oe-port 6f47d2c48, S22 and S14b, merged; d32267af6 and the docs after it):
`WERROR=1 tools/build.sh linux`; `gen_tool_docs.py` (229 tools, no change); htmlui glcore and Vulkan
3,480 ran, 0 failed each (B); unit tests core 1,419 and editor 883 ran, 0 failed (B); MCP
(the files above): 345 passed, 3 failed, the same three that fail without S16;
`test_html_ui_phase2::test_mistakes_are_reported` (four copies at once, five rounds) 0 of 20
failed with the screenshot's HTML document wait, 3 of 20 without it; smoke (glcore and Vulkan,
the installed engine copies removed first), web + web-gate pass (C).

Landing gate on oe-port 424925911 (S26, BF5, S08 merged, 7dc3a625e and after): the Xcode project
parses with no dangling reference or duplicate id and every `xcode_sync_*.py --check` passes (its
msdfgen core group had lost its closing lines in the S17 merge; `xcode_sync_s16.py --check` now
parses the file); `WERROR=1 tools/build.sh linux`; htmlui glcore and Vulkan 3,480 ran, 0 failed
each, unit tests core 1,452 and editor 1,029 ran, 0 failed (B); MCP (the files above with
`test_docs.py` and `test_reflection.py`): the three failures without S16, and two of S16 fixed
since: the default listing 22 characters over its 100,000 with S26's and S08's tools (`inspect_ui`
44 shorter) and S08's reflection goldens without S16's project settings, generated
`setHtmlUiRenderOptions` and `htmluiprim` shader key (updated, nothing else in them changed);
their rerun (`test_reflection`, `test_tool_budget`, `test_html_ui_tools`, `test_tool_conventions`,
`test_docs`, `test_protocol`): 73 passed, 0 failed; `gen_tool_docs.py` 236 tools; smoke (glcore
and Vulkan, on the host), web + web-gate pass (C).
