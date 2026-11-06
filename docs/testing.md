# Testing JasperEngine

Every change ships with tests, and every suite runs the same way on any machine: inside the Docker images under `docker/`, through `tools/build.sh`. This page lists
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
| Android packaging | what Build makes for Android, run for real: APK and App Bundle from the prebuilt player, the bundletool download (real URL), the debug and release keys, wrong passwords, `publish prepare`, then both installed on the emulator and their screens compared with the golden | `tools/ci/android_packaging.sh` (needs `tools/build.sh linux`, the Android player; KVM: server A) | `build/android-packaging/` |
| iOS packaging | what Build makes for iOS on a Mac: the Xcode project through `xcodebuild`, installed and launched on a simulator device of its own (no window), a device build as far as signing, the IPA path without a developer account, `publish prepare` | `tools/ci/ios_packaging.sh` (needs the editor on a Mac with Xcode, `tools/players/ios_player.sh`; `IOS_PACKAGING_GOLDEN=1` compares the screen with `tools/fixtures/unified-lighting/golden/ios/screen.png`, from an iPhone 17 simulator) | `build/ios-packaging/` |
| Tool checks | the CI scripts' own tests, the presets, the HTML UI golden ledger | `python3 tools/ci/test_gtest_gate.py`, `python3 tools/ci/test_smoke_jit_hangs.py`, `python3 tools/ci/test_android_screen_check.py`, `python3 tools/ci/test_check_notices.py`, `python3 tools/ci/test_check_golden_ledger.py`, `python3 tools/htmlui/check_golden_ledger.py`, `python3 tools/check-editor-modals.py` (`--self-test`), `tools/build.sh exec linux tools/ci/check-presets.sh`, `python3 tools/procmat/test_import_mm_nodes.py` (S90's node importer: licences, the deny-list, what is data) | stdout |

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
WERROR=1 tools/build.sh unit-tests
RUN=0 tools/build.sh unit-tests                             # build only
```

The binaries also register with CTest (`ctest --preset linux-ci` in an IDE build), labelled
`unit`, `gpu` and `slow` by suite name.

Picking (S26): `JASPER_PICK_SIMD=scalar|avx2|neon` forces the mesh BVH's slab test path; `MeshPose.MatchesGpuSkinning` is a GPU suite (the
CPU pose against the skinned shaders, glcore and Vulkan). `JASPER_PICK_BUILD_DELAY_MS=<ms>` holds
every background mesh BVH build pending that long: the MCP tests use it to see the Scene view's
waiting click and refused drop (`tools/mcp/tests/test_scene_view*.py`).
`JASPER_TEXTURE_LOAD_DELAY_MS=<ms>` holds every async texture decode that long: a screenshot must
then wait for the texture, never show its stand-in (`tools/mcp/tests/test_screenshot_waits.py`).
`JASPER_SHADER_LIST_SYNC_MS=<ms>` is the time `get_build_info` may read closed scenes before it hands
the rest to a job; 0 always hands them over (`tools/mcp/tests/test_shader_lists.py`).
`JASPER_SHADER_LIST_WRITE_HOLD_FILE=<path>` holds the write of that cache while the file exists, up to 20 s (same test).
`JASPER_TEST_RECORDER_FAIL=<list>` forces Recorder failures (`editor/recorder/RecorderSettings.h`): `audio@N`,
`audio-header`, `video@N`, `encoder-kill@N`, `mux`, `png@N`, `publish` (they read as a full disk),
`hold-limit=<ms>` (the wait for a frame's loads) and `free=<MB>` (the free space seen); read once per
recording, nothing when unset (`tools/mcp/tests/test_recorder_integrity.py`).
`JASPER_MESH_LOAD_HOLD_FILE=<path>` holds the first load (the binding) of the meshes of a scene that
loaded a model in the background while that file exists, so what else loads the same model meanwhile
(a Project window thumbnail's preview) comes first (`tools/mcp/tests/test_model_texture_bind.py`).
`JASPER_TEST_MATERIAL_THUMBNAIL_STUCK=<project-relative file>` keeps a material thumbnail loading while that file exists (`test_material_thumbnail.py`).
`JASPER_TEST_STAGING_HOLD=<path>` holds a Project Hub import or template copy with part of its files in
its staging folder while that file exists, after creating `<path>.held`: the tests kill the editor
there (`CrashLeftovers.*` in `editor/tests/hub/HubTest.cpp`, `tools/mcp/tests/test_crash_leftovers.py`).

### HTML UI, MCP, smoke, web

- `tools/build.sh htmlui-tests`: about 3,300 tests, 74 of them skipped on purpose (the gate allows
  exactly those). `HTMLUI_TEST_ARGS` filters, `JASPER_HTMLUI_PERF_SLACK` relaxes the performance
  targets on a loaded host; `MarkdownHtml.Perf20KB` is enforced only when it is set
  (below, Writing load-robust tests).
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
  `CI = True` (`tools/ci/check-mcp-ci-suite.py`, run by check-changed: every file is classified,
  docs/mcp/README.md); `MCP_SUITE=all` runs every file and needs the default fixture project, which is
  not in git: `JASPER_FIXTURE`, else `build/fixture-unity` of the checkout or the main checkout,
  else `~/je/fixture-unity` (the build servers' copy), mounted at `/fixture`. The harness looks in
  the same places (`harness.default_fixture()`); without a fixture, the tests that need it skip
  and say where they looked. `MCP_SUITE=all` first builds the mesh-raycast web test project
  (`build/web-mesh-raycast`, the web image; up to date after the first time) for
  `test_mesh_raycast.py`'s web latency gate. The Linux image carries the suite's test tools
  (lua-language-server, clangd, FFmpeg with AV1, chrome-headless-shell), so none of their tests
  skips there. See `docs/mcp/README.md` for writing MCP tests.
- `test_audio_meter.py` takes no editor: it compiles `engine/tests/core/audio/AudioMeterProbe.cpp` with
  `AudioMeter.cpp` and `TruePeak.cpp` alone (so it needs a C++ compiler; FFmpeg decodes the fixtures)
  and compares the meter with pyloudnorm (`uv run --with`, pinned in the test).
- An MCP test asserts behaviour; a wall-clock budget (a CLI start in 300 ms, a reload frame under
  16 ms, ...) goes through `harness.over_budget(what, measures, budget)`, which prints a `[bench]`
  line and fails only when `JASPER_MCP_PERF_SLACK` is set: a shared build server's load alone
  misses those budgets. The benchmark run enforces them on a quiet machine, with nothing else running:
  `JASPER_MCP_PERF_SLACK=1 tools/build.sh mcp-tests tools/mcp/tests/test_call_cli.py
  tools/mcp/tests/test_hot_reload.py tools/mcp/tests/test_checks.py`.
- Compiled and prebuilt game builds: see the next section.
- With `-j N` the runner starts the longest files first. It orders them by each file's wall time
  in the last unfiltered run (`build/test-results/mcp-file-seconds.json`); files with no time yet
  go first. So no long file starts last and alone. Each file has its own process, editor and
  scratch directory, so the order changes nothing a test sees.
- `tools/smoke-test.sh <project>`: see its header. It also checks that the exported project
  configures as a user's game does: no presets file, no warnings as errors, no sanitizer.
  A hang watchdog report fails it, with one exception: on a project's first editor open, when every
  report's stack is llvmpipe's JIT inside a draw (`tools/ci/smoke_jit_hangs.py`), it opens the editor
  again with the Mesa disk cache warm, and that open must have no hang at all. A software GL compiles
  a program's variant at its first draw on the render thread (S32 deviation 6), and a scene of many Lit
  variants holds that frame 7-9 s (`tools/fixtures/pbr-extended-export`; S32, Progress).
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

### Compiled and prebuilt game builds (MCP, `JASPER_SLOW_BUILD=1`)

Compiling a game (Build Method Compile: CMake, then the engine with the game) takes 20 to 60
minutes on a build server. Building one with a prebuilt player runtime (S30, `docs/shipping.md`)
takes about a second. So an MCP test compiles only when the compile path is what it checks:

- **(a) Compiled** (`build_project ... method="compile"`, and it checks `method_used` is
  `compile`). A deliberate, minimal set, at least one for each target and backend the suite
  compiles (Linux desktop, glcore):

  | Test | Why it compiles |
  |---|---|
  | `test_build.py::test_desktop_build_and_run` | the compile path end to end (CMake, the engine, the link, a player that runs), and a broken C++ source's errors parsed as `file:line` (only a compiler produces them) |
  | `test_html_ui_effects_export.py::test_desktop_player_draws_the_effects` | `graphics.runtime_shaders: false`: a compiled game ships without the runtime shader compiler whatever its scripts do (a prebuilt variant leaves it out only when no script can need it, S34), so it shows the exported UI effect variants are all the game needs |
  | `test_build.py::test_cancel_desktop_build`, `test_busy.py::test_export_job_refusals` | a compile runs long enough to cancel, and to refuse other tools while it runs (a prebuilt build ends first); both are cancelled within seconds |

- **(b) Prebuilt** (`method="prebuilt"`, and it checks `method_used` and the `player_id` it
  used). The test needs a real game that runs, not the compile:
  `test_build.py::test_development_build_crash_report` (the Development runtime crashes on
  purpose and writes its report), `test_html_ui_export.py::test_desktop_build_runs_the_document`,
  `test_html_ui_video_export.py::test_desktop_build_plays_the_video` and
  `test_call_cli.py::test_wait_job_build` (the CLI's `--wait-job` on a build job). The runtime is
  the one `tools/build.sh players` builds, from the same engine sources as the editor under
  test.

- **(c) Shared** builds: a runtime is built once per suite run for each backend and flavour, and
  every test and `-j` worker shares it (`tools/mcp/build_cache.py`). A file declares the runtimes
  its tests use (`PREBUILT_PLAYERS = ["glcore-release"]`). With `JASPER_SLOW_BUILD=1` and no
  `-k`, the runner starts building them as the run starts. It starts a file that needs one only
once that runtime is built, and runs the other files meanwhile, so no worker waits for a build. A test gets one
  with `await build_cache.player_runtime(ed, "glcore", "release")` (it waits under `ed.busy()`
  while another worker builds it) and points its editor at it with
  `env=build_cache.player_env("glcore", "release")` (`JASPER_PLAYERS_DIR`; set it for the editors
  that build with it only: a players directory changes what Build Method Auto picks).

The cache cannot hide a bug:

- Its key covers everything a runtime is built from. That is the content (not the commit or the
  file times) of every file under `engine/`, `tools/players/` and `tools/performance/`, plus
  `tools/ci/check-notices.py`, `NOTICE.md`, `LICENSE`, the Linux image's `docker/linux.Dockerfile`
  and the cache's own source. It also covers the exact CMake arguments, the compiler, linker,
  CMake and Ninja versions, the compiler environment (`CC`, `CXX`, `CFLAGS`, ...), and the editor
  version the runtime is locked to. `python3 tools/mcp/build_cache.py key glcore release` prints it.
- The runtimes are a test's: no GCC link-time optimisation (`-DJASPERENGINE_RELEASE_LTO=OFF`, in
  the key); the rest of the release link (sections, hidden visibility, split symbols) is as
  shipped. LTO was most of a release runtime's build (its link alone a quarter of an hour on a
  loaded server) and changes nothing a test of the game checks. The suite keeps one LTO check:
  `test_build.py::test_desktop_build_and_run` compiles a shipped game's release link with LTO, runs
  the player and checks its compile tree was configured with `JASPERENGINE_RELEASE_LINK` and
  `JASPERENGINE_RELEASE_LTO` on; `tools/build.sh players` and `tools/build.sh linux`'s sample
  project (the gate's WERROR build) link with LTO too.
- **Shared per machine** (`JASPER_MCP_SHARED_BUILD_CACHE=<dir>`, which `tools/build.sh mcp-tests` mounts at
  `/mcp-build-cache`):
  one entry per runtime and key, `<dir>/v1/<runtime id>/<key>/`, used by every worktree, stream and
  run of the machine. One process builds a key (an exclusive lock), every other one that needs it
  waits for that build; the build goes to a staging directory renamed into place complete, so no
  reader sees half an entry. A run holds a shared lock on each entry it uses for as long as it
  runs; the size limit (`JASPER_MCP_BUILD_CACHE_MAX_GB`, default 15) evicts the least recently
  used entries nobody holds, and the staging directories of builds that died.
  `python3 tools/mcp/build_cache.py list` (or `evict`) shows them; `status` says whether this
  checkout's runtimes are built. The run's `build/linux/mcp-build-cache/<runtime id>/players` is a
  link to its entry's.
- Without a shared cache it lives under the run's build directory, next to the editor under test:
  `build/linux/mcp-build-cache/<runtime id>/` (`players/`, `key.json`, `build.log`;
  `JASPER_MCP_BUILD_CACHE` moves it), and an entry whose key differs from the current one is
  deleted before anything is built.
- Either way a runtime of other sources (another commit, an uncommitted edit) is never reused.
- Each build starts from an empty CMake tree at a fixed path (`build/linux/mcp-build-cache/<runtime
  id>/tree`, `/src/...` in the image whatever the worktree), through
  `tools/players/build_linux_player.sh` (what `tools/build.sh players` runs). Below it, ccache
  (`CCACHE_DIR`, as for the editor itself) keys each object on its preprocessed source, compiler,
  flags and directory; the build's hits and misses go in its `key.json` and in the run's output.
- A failed build fails every test that needs it, with its log, for the rest of the run. The next
  run tries again.
- The compiled games (`method="compile"`) build through ccache too: their editors get
  `harness.compile_env(name)`, a user cache (where the kit's compile tree goes) at the fixed path
  `build/mcp-compile-home/<file>/`, emptied as the file starts, and ccache as CMake's compiler
  launcher. Each run still configures and compiles a fresh tree and links; an object comes from
  ccache only when its preprocessed source, flags and directory are those it was made from. They
  link without LTO (`JASPERENGINE_RELEASE_LTO=OFF` in the editor's environment, which an exported
  game's first configure takes as the option's default) except `test_build.py`'s (`lto=True`).

A new test that needs a game that runs builds it prebuilt. One that needs what only a compiled
game has (C++ scripts, `ProjectBuild.cmake`, capacities above the player's, AdMob, no runtime
shader compiler, compiler output) compiles, and joins the table above with its reason.

### Linked folders in a web game (S68 AC2)

`tools/mcp/tests/test_linked_folders_export.py` builds and runs a Desktop game with
`JASPER_SLOW_BUILD=1`. A web game needs the web image, which has no editor, so it takes two steps:
`JASPER_LINKED_WEB_EXPORT=build/linked-web tools/mcp/run-tests.sh tools/mcp/tests/test_linked_folders_export.py -k web_export` leaves
the source export there (the links resolved into real files), then `tools/web/linked_folder_game.sh build/linked-web`, in the web
image, builds it and runs `tools/web/browser_gate.py` on it with `tools/web/gates/linked-folder-game.json`:
the centre pixel is the linked texture's red and the console has the linked Lua module's line.

### Test projects

`tools/test-projects/<name>/` are small engine projects (`main.cpp`, `lua/`, `assets/`) that a
platform test builds instead of `engine/project`: `WEB_PROJECT=<name> tools/build.sh web`
(`build/web-<name>`, checked by `GATE=<name> tools/build.sh web-gate` with
`tools/web/gates/<name>.json`) and `ANDROID_PROJECT=<name> tools/build.sh android`
(`build/android-<name>/app-debug.apk`, checked by the emulator runner below). Keep one project per
behaviour under test, with its expectations next to it.

### Android emulator: `tools/android-emulator-test.sh`

Installs an APK on a headless Android emulator, launches it with its LAUNCHER intent and checks
its logcat. It needs KVM: run it on a Linux host with `/dev/kvm`.

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
  about 40 s; a whole run about 70 s.
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
ANDROID_PROJECT=render-gate ANDROID_ABIS=x86_64 tools/build.sh android
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

For Android, build `ANDROID_PROJECT=crash-diagnostics tools/build.sh android`,
then `tools/android-emulator-test.sh build/android-crash-diagnostics/app-debug.apk
--expect-native-crash --expect 'JASPERENGINE CRASH:'`. This fixture segfaults at frame 10.
The opt-in flag requires the engine report **and** a matching debuggerd tombstone, saves both
under the emulator result directory, and accepts that deliberate crash. Ordinary emulator tests
continue to fail on any native crash.

## Gates: per commit, before pushing to main

- **Per commit of a stream: `tools/check-changed`**. It builds and runs what the change can affect: the Linux editor and the
  unit test binaries for C++ under `engine/` or `editor/` (web for the engine, Android for its own
  code), the gtest suites whose test units include a changed file (Ninja's dependency database:
  a changed `Foo.cpp` counts through `Foo.h` and everything that includes it), and the MCP test
  files the coverage map ties to the change (below). Everything for the root CMake, `docker/`,
  `tools/build.sh`, the toolchain and the lockfiles; the whole MCP suite for a change of its harness
  (`harness.py`, `run_tests.py`, `build_cache.py`); nothing for docs. A header under `engine/core` or
  `engine/CMakeLists.txt` also builds the engine with every optional module off
  (`tools/check-physics-off.sh all-off`): the editor's engine has every module, so only this shows a
  source that got a header through a module's code, which breaks Compile builds and the smaller players.
- **Before pushing to `origin main`, only when the pushed changes need it (a big feature, not a
  small fix): the full suite** (`MCP_SUITE=all JASPER_SLOW_BUILD=1
  JASPER_SLOW_TESTS=1`), plus the WERROR Linux
  build (with the engine sample project's LTO link) and every unit test. check-changed is a
  selection and says so in every summary.

### `tools/check-changed`

```sh
tools/check-changed                           # build and test what the change touches
tools/check-changed --dry-run                 # the plan only
tools/check-changed --base main               # another base (default: the merge base with oe-port, else main, origin/main)
tools/check-changed --no-mcp                  # builds and unit tests only, said so in the summary and not cached
tools/build.sh exec linux tools/check-changed # inside the Linux image (no web or Android build there)
```

- The change: committed since the base, staged, unstaged and untracked files. A checkout without
  the stream's history reads its diff from `build/.rb-changes.json` (with every file's sha256: a
  diff that does not match the files is refused).
- Unit tests: the gtest suites (`TEST`, `TEST_F`, `TEST_P`, typed) of the test units that see the
  change, in `jasperengine-core-tests` and `jasperengine-editor-tests` (`--gtest_filter`, through
  `tools/build.sh unit-tests` and its no-silent-green gate); a changed test file always runs; a
  CMake change, or test trees not built yet, run them all.
- MCP tests: the files that executed the functions the diff's hunks touch (coverage map: the whole
  file when a changed line is outside every function, a declaration or a type; a header then also
  through its `.cpp`; only new lines outside the functions touch nothing old, a new function counts
  through its callers' changes; the lines reach the map's through the diff from its commit; a
  function nearly every editor runs selects the 10 quickest files that run it), the changed test files, the files that call a
  changed MCP tool (by its tool names), that load a changed fixture, test project, Lua script,
  shader, HTML, scene or material (by path), and, for an engine change, the files that run a prebuilt
  game (`GAME_BUILD_FILES`: it runs engine code no editor coverage sees), those that compile a
  game when what the compile path reads changes (`COMPILE_PATHS`), and the guard of what a save
  writes (`test_saved_output_determinism.py`, spec S63) when serialization or code generation
  changes (`SAVED_OUTPUT_PATHS`).
  No map, a map older than 14 days or 400 commits, or a changed source the map does not know (new
  code), or a file no test names adds the CI suite; code the map knows that no test executes
  selects nothing, and the summary says so. A selected test that skips
  could not run: that fails the check.
- The diff is the stream's, so a long or wide stream selects most of the suite (a tool like
  `create_entity` or the frame reaches most test files). `--base HEAD~1` checks one commit, when the
  one before it passed.
- A pass is cached in `build/.cache/check-changed.json`, keyed by the base, the content of every
  changed and untracked file, the images' Dockerfiles, the coverage map's version and the script
  itself: the same inputs reprint the last summary (`--no-cache` runs again); a run during which
  the files changed caches nothing. Exit 1 when a step fails or a selected test cannot run.
- Output: the plan, then each step's result; a failed step's output (`CHECK_CHANGED_VERBOSE=1`:
  everything).
- Inside the Linux image `tools/build.sh` has no Docker to call and runs the image's own targets
  directly, with the same environment; a target of another image (web, Android) exits 3; check-changed there leaves those
  builds out, says so in its summary and caches no pass.
- Its tests: `python3 -I tools/test_check_changed.py` (the diff and its fallbacks, the build plan,
  the suites from a synthetic `ninja -t deps`, the MCP selection from a synthetic map, the gcda
  reader, the cache and its invalidation, the exit codes).

**The coverage map** (`tools/mcp/coverage_map.py`): for each MCP test file, the engine and editor
sources its editors executed. A full run of a coverage-instrumented editor records it, off the
critical path (a full suite run, a quiet machine); it is stored with its commit in the machine's
`~/je/mcp-build-cache/coverage-map.json` (check-changed finds it there, or
`JASPER_MCP_COVERAGE_MAP`):

```sh
COVERAGE=1 SKIP_ENGINE=1 tools/build.sh linux          # build/linux/editor-coverage-install
JASPER_MCP_SHARED_BUILD_CACHE=~/je/mcp-build-cache tools/build.sh exec linux bash -c 'JASPER_EDITOR=/src/build/linux/editor-coverage-install/bin/jasperengine-editor \
  JASPER_MCP_COVERAGE_DIR=/src/build/coverage/runs JASPER_SLOW_TESTS=1 \
  dbus-run-session -- tools/mcp/run-tests.sh -j 4 --split-seconds 900; \
  python3 tools/mcp/coverage_map.py collect build/coverage/runs build/linux/editor-coverage \
  --commit <the commit> --out /mcp-build-cache/coverage-map.json'
```

Each test file's (and chunk's) editors write their GCC counts under their own `GCOV_PREFIX`; a
function counts for a test file when a counter of it is above zero; the object's `.gcno` says which file
and lines it is written in (a header's for an inline function), which takes check-changed from a
diff's hunks to functions. The run leaves out the slow builds
(no `JASPER_SLOW_BUILD`): what a compile or a prebuilt game runs is not the editor's code, and the
files that build games are selected by path (`GAME_BUILD_FILES`, `COMPILE_PATHS`).

### Splitting the full suite

A file's tests share an editor and run in order, so a file is one piece of work, unless it declares
`TESTS_INDEPENDENT = "why"` (each test sets up what it needs and runs in an editor of its own, in
any order) or `TESTS_INDEPENDENT = ("why", ["test_x_*"])` for some of its tests (those use nothing
the file's other tests leave, nor do the others use theirs): their tests may be split into chunks,
each with its own worker process and editor (`run_tests.py --split-seconds N`). `test_html_ui_examples.py` (each example's golden) and the
Khronos models of `test_materials_pbr.py` (`test_khronos_<Model>`) declare it.

### Caches and CPU caps

- **ccache** is shared by every worktree of a machine (`<main checkout>/build/.cache/ccache`), and
  every worktree is `/src` in the image, so a tree another worktree built is a hit:
  an editor build of oe-port in a new worktree on A, 2,144 direct hits of 2,150 compiles, 158 s.
  What a merge changes is compiled once per machine.
- **The prebuilt player runtimes**: once per content key per machine (above).
- **Mesa's shader cache**: llvmpipe and lavapipe compile every shader to machine code; the editors
  keep that cache (`MESA_SHADER_CACHE_DIR`) in the server's shared MCP cache (or the checkout's
  `build/.cache/mesa-shader-cache`) instead of a fresh one in every scratch HOME. It is Mesa's own
  cache, keyed by the shader and the driver build, on by default for every Mesa user.
- **`LP_NUM_THREADS`**: Mesa's llvmpipe gives every editor a rasterizer thread per CPU of the host,
  not of the container's quota; at `-j N` that is N times the host's CPUs inside the quota, which
  throttles them all at once, main threads included (the 3-13 s stalls on A). The harness gives
  each editor (and the players it runs) 4 threads (`harness.render_threads()`: fewer, down to 2,
  only at under half a CPU per thread; `JASPER_MCP_RENDER_THREADS`, or `LP_NUM_THREADS` in the
  run's environment, overrides). Measured on A, the same 36 tests at `-j 4` in an 8-CPU container
  (the host shared with other jobs; the last run's host was the busiest):

  | `LP_NUM_THREADS` | Wall | Median s per frame | Host load, median / peak | Result |
  |---|---|---|---|---|
  | Mesa's default (16) | 2,009 s | 1.63 | 21 / 53 | 36 passed |
  | 4 | 1,772 s | 1.46 | 12 / 23 | 36 passed |
  | 2 | 2,517 s | 2.22 | not sampled | 35 passed, a screenshot timed out |

  An editor averages about 2 CPUs and holds 1 to 2.4 GB (seven at once: 6.8 GB), so memory does not
  bound `-j`: CPUs do, at about one editor per 2 CPUs of quota (`-j 4` on 8, `-j 7` on 16).

### The real-GPU lane

The GPU lane runs the tests whose checks hold on any renderer on an NVIDIA GPU, through headless
NVIDIA X servers in their own containers (`tools/gpu/xorg-headless.sh`, `JE_GPU_SCREEN=1600x900`,
the size of an editor's own Xvfb; the desktop session is never touched): `DOCKER_GPU=1
tools/build.sh mcp-tests --plan ...` sources `tools/gpu/env.sh`, and the runner gives a chunk of
the lane `gpu` the NVIDIA display (one display per chunk at a time, `JASPER_MCP_GPU_DISPLAYS`; the
harness's own X server is that display too, for the xdotool tests) and every other chunk llvmpipe
as anywhere. Each editor checks it renders where its chunk says (`JASPER_MCP_EXPECT_DEVICE`:
NVIDIA, or llvmpipe), so a lane test never runs on llvmpipe unnoticed and no golden test runs on
the GPU. `tools/mcp/gpu_lane.json` classes every graphics test file:

| Class | Files | What |
|---|---|---|
| (a) `gpu` | 43 | relative or structural checks (a change between two renders, no black or NaN pixels, values, undo, saved files); each passed twice on the RTX; BF16's 34 together 7,259 s of test time on llvmpipe under load, 1,223 s on the RTX: `animation_clip`, `animation_inspector`, `animation_record`, `animation_window`, `animator`, `async_shaders`, `atmosphere`, `atmosphere_editor`, `autosync`, `bake_vertex_animation`, `buoyancy`, `core_utilities`, `ecs`, `ecs_collision`, `fluid_agent`, `fluid_fixed`, `hot_reload`, `html_ui_docs`, `html_ui_tools`, `html_ui_video`, `html_ui_widgets`, `play`, `play_stop_gizmos`, `pose_animation`, `pose_runtime`, `post_fx_settings`, `scene_view_ui`, `shader_library_phase2_review`, `shadows_ui`, `stochastic_tiling`, `undo_ui`, `vat_baker_window`, `vat_inspector`, `water`; since BF20: `fluid_effect`, `fluid_inspector`, `html_ui_effects`, `lattice_bake`, `place_with_physics`, `pose_render`, `pose_window`, `reflection`, `upgrade_from_main` |
| (b) `llvmpipe` | 8 | compare with goldens drawn by llvmpipe, and the MCP suite has no per-renderer goldens (the unit tests' `isReferenceRenderer` and `hardwareTolerance` are C++ only), so they stay the reference: `html_ui_examples`, `lattice`, `materials_pbr`, `navigation`, `particles_tools`, `post_fx_golden`, `scene_view`, `shadows` |
| (c) `other`, not on the lane | 77 | budgets or frame times of the software renderer, game builds, no renderer in use, or under a minute with nothing to gain (not validated) |

Lane files run on one display per chunk, two at a time; a file holding one test that is not GPU-OK stays on llvmpipe whole, because its tests share an editor. Files not listed render nothing that matters and stay on llvmpipe.

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
  `JASPER_REQUIRE_GPU()`: without a device it fails, it never skips. GPU suites run on Linux
  (Xvfb) and on macOS (the default Metal device: `HeadlessContextMetal.mm`, `ReadbackMetal.mm`);
  Run only the CPU suites where no device exists with `--gtest_filter=-*Gpu*`.
- A suite whose tests take more than 5 s each ends in `Slow`. Any other test stays under 1 s, and
  both binaries' CPU suites together under 60 s on 4 cores.
- CTest labels follow the names: `gpu`, `slow`, else `unit`.
- `ReflectParity.*` compares the reflection codec against frozen data
  (`editor/tests/reflect/parity_golden/`): a field added to a reflection table adds its key there,
  and `JASPER_TEST_UPDATE_GOLDEN=1` rewrites the files. `AssetRefSchemaFrozen.*` holds what the hand
  table of asset references gave (`editor/tests/asset/asset_ref_*_frozen.txt`); those files are not
  rewritten, a new reference field only adds rows.

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
  their budgets that way and print `[bench]` lines (S34's `LuaBindingBench` prints the cost of a call
  through the Lua bindings, with no budget: builds are compared with it on the benchmark server); the
  editor suite's `editor::test::perfSlack()` does the same for `CommandHistoryBench` (S24),
  `AutosaveEncodeBench` (S03), `VcsBench` (S29: status poll, snapshot swap, scene diff; the
  main-thread encode is reported), `ChangedUpdateFlagsBench` (S71: 50 us a call per component) and
  `ReflectCodecBench`/`ReflectResolveBench` (S08; its MCP R19 frame
  cost, `test_reflection.py::test_inspector_frame_cost`, reads the same variable) (ratios such as a report's overhead stay checked everywhere). The MCP
  texture storm's frame budget (`test_jobs_memory_ui.py`, S06 AC23) reads the same variable; it assumes
  a machine whose decode workers and software renderer do not share cores (8+ hardware threads):
  E's 4 vCPU measure a storm p95 of 1.7x the idle frame against the 1.5x budget, so it runs there
  only with a slack above that. A
  shared build server's load alone misses them (10k timers in 20 ms instead of 2), so an ordinary
  unit test run never fails on wall-clock time; the benchmark run enforces them on a
  quiet machine, with nothing else running:

  ```bash
  JASPER_TEST_PERF_SLACK=1 \
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
  allowed (0, except the HTML UI suite's recorded 74). It lists every disabled test. The test targets
  go through it; `tools/ci/test_gtest_gate.py` tests it.
- Declared tests are the tests that ran plus the disabled ones. A test that flakes twice in a week
  is quarantined with a `DISABLED_` prefix and a tracking issue; the gate's list keeps it visible.
  Opt-in tests that need what the repository does not hold (`VatImportSideFx.*`: the SideFX VAT 3
  tutorial exports, `JASPER_VAT_SIDEFX_EXAMPLES`) or are measurements to run by hand
  (`EntityDestroyScaling.Profile`, `JASPER_DESTROY_PROFILE`) are `DISABLED_` too, never a skip:
  `--gtest_also_run_disabled_tests` with the filter runs them.
- A behavioural claim needs a red control: revert the behaviour locally, show the test failing
  (paste the output into the pull request), restore it. A bug fix shows the test red before the
  fix and green after.
- The XML stays in `build/test-results/`.
- The first tests of an area are characterization tests: they pin today's behaviour. A change
  that alters that behaviour on purpose updates those tests in the same commit and says so.

## Writing load-robust tests

The suites can run on shared machines, next to builds and other suites (`-j 3`): there a frame
of a software-rendered editor or player takes 0.1 to 0.3 s, a background job seconds, the main
thread misses a 1 s watchdog threshold on its own. A test that passes only on a quiet machine
hides real regressions behind re-runs. So (BF12):

- **Wait for the state, not for time.** Ask for the condition until it holds; the deadline is only
  a hang guard, never the expected duration. `test_crash.py::test_health_off_main` asks
  `get_editor_health` until it reports the hang, where a fixed sleep missed its start.
- **Game time, not wall time.** In Play, `play start` with `fixed_fps` and `auto_pause` runs the
  game only inside `wait` / `send_input`, each frame exactly 1/N s, and
  `wait {"until": ..., "frames": cap}` bounds it in frames. A probe that samples a window of game
  time narrower than a frame step skips it at a low frame rate (Engine caps a frame at 0.25 s):
  `test_lattice_anim.py` sampled clip time 0.3 to 0.5 s, which 4 fps stepped over. A player:
  `run_build` with `after_ready`, `fixed_fps` and `quit_after` (or `screenshot_frame`) captures
  the player's own frame N frames after it got ready, not a window N seconds after the launch.
- **A screenshot is of loaded content.** The editor's `screenshot`, the player's `--screenshot`
  and the readiness line wait for shaders being made and textures loading (a frame drawn while a
  texture loads shows a stand-in for it) and fail naming them past their timeout, so a golden
  comparison never meets a stand-in.
- **Make preconditions explicit.** Background work a test relies on finishes first:
  `HotReloadServiceTests.ExternalRenameKeepsTheGuidThroughTheWatcher` calls
  `AssetDatabase::waitForBackgroundWork()` before the rename, because R5 matches a file moved
  without its meta by the content hash the background hasher computes.
- **Pick records by what identifies them.** A loaded machine produces real events of its own (a
  stall past the threshold is a real hang report): find the debug hang's report by its phase, not
  as the first or the newest.
- **Keep random parts out of assertions.** Match file names, not full paths under a random temp
  folder (`FileWatcherTests`: `je-watch-<random>` holds "4913", vim's probe name, one time in 600).
- **A budget is a benchmark.** MCP tests report wall-clock budgets with `harness.over_budget`,
  units with `perfSlack()` (`JASPER_TEST_PERF_SLACK`); both are enforced by the benchmark run
  (C, `RB_SLOTS=2`). The HTML UI gates take the least of several CPU-time measurements of the calling
  thread (`PerfFixtures.h`: `gatedCpuMs`); one whose budget is near the step's cost on a build
  server (`MarkdownHtml.Perf20KB`: 1.3 ms of 2 there, 0.27 ms on a quiet one) is enforced only when
  `JASPER_HTMLUI_PERF_SLACK` is set (`benchmarkPerfSlack()`):
  `JASPER_HTMLUI_PERF_SLACK=1 HTMLUI_TEST_ARGS='--gtest_filter=MarkdownHtml.Perf20KB' tools/build.sh htmlui-tests`.
  Scaling a timeout with a load factor is the last resort, and a test never asserts less to pass.
- **Prove it under load.** Run the test where it failed, under deliberate load, before and after:
  `--gtest_repeat=50` for a unit test, 20 runs for an MCP test, in parallel lanes next to busy
  loops in the image (`tools/build.sh exec linux ...`; the container's `--cpus` keeps the load off
  other jobs) or next to a full build. The run on the unchanged branch is the red control.

## How a spec adds tests

1. Put unit tests where the table above says, named as above. Use `TempDir` for files and
   `EXPECT_NO_ENGINE_ERRORS()` where the engine logs.
2. Rendering changes add a `*Gpu` test with a golden in `engine/tests/core/golden/`, created on
   glcore and matched on Vulkan.
3. Agent-facing features add MCP tests, CI-capable (`CI = True`, own project).
4. Run `tools/build.sh unit-tests` (and the suites of the touched area); record
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
| `JASPERENGINE_PCH` | ON (editor tree), OFF (`engine/`) | precompiled headers of our own targets, GCC and Clang (below) |
| `JASPERENGINE_LINKER` | AUTO | lld for Debug and RelWithDebInfo builds that ship nothing; `DEFAULT`, `LLD`, `MOLD`, `GOLD` |
| `JASPERENGINE_VENDORED_UNITY` | ON, OFF for a shipped game | Jolt unity-built |

`engine/CMakeLists.txt` is also the top-level file of every exported game, so all of these default
to off there, and the exporter never sets them: a game configures and builds exactly as before.

### Sanitizers

```sh
SANITIZE=address,undefined tools/build.sh unit-tests   # build/linux/core-tests-address-undefined, Debug
SANITIZE=thread UNIT_TEST_ARGS='--gtest_filter=-*Gpu*' tools/build.sh unit-tests
SANITIZE=address,undefined tools/build.sh linux        # an instrumented editor: build/linux/editor-address-undefined(-install)
SANITIZE=address,undefined tools/build.sh mcp-tests tools/mcp/tests/test_protocol.py   # MCP tests on that install
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

A sanitizer `linux` build installs like a normal one, unstripped (the reports need the symbols),
into `build/linux/editor-<sanitizers>-install`: its build type is Debug by default, so the install
rules' configurations (`JASPERENGINE_INSTALL_CONFIGURATIONS`) add Debug and RelWithDebInfo when
`JASPERENGINE_SANITIZE` is on. `SANITIZE=... tools/build.sh mcp-tests` runs the harness on that install
(`JASPER_EDITOR` names it, with TSan's `setarch` wrapper); a sanitizer report ends the editor, and so the test.
Leak checking is off in that run: the editor's exit reports what Xvfb's unloaded GL driver kept, frames of an
unmapped module that no suppression can name. The unit tests hold the leak check.

`tools/build.sh` passes these runtime options on every run:
`ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:strict_string_checks=1`,
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:suppressions=tools/sanitizers/ubsan.supp`,
`LSAN_OPTIONS=suppressions=tools/sanitizers/lsan.supp`,
`TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=tools/sanitizers/tsan.supp`.
The suppression files list third-party code only (Mesa, X11, fontconfig, vendored libraries),
each entry with its reason. Engine and editor code is never suppressed: fix it.

GCC 11's TSan (the Linux image) does not intercept `pthread_cond_clockwait` or
`pthread_mutex_clocklock`, which `std::condition_variable::wait_for` and `std::timed_mutex` lock
through on the steady clock: one such wait makes its mutex look held, reports double locks there,
and, since a `std::mutex` never tells TSan it is gone, breaks the next mutex at its address (reports
of races under a mutex both sides hold). Core timed waits go through `jobs::detail::timedWait`
(`jobs/detail/TimedWait.h`), a lock with a deadline is a `try_lock` loop (`Logger.cpp`, `lockUntil`;
`io/watch/backends/Backends.h`, `lockWithin`; the editor's timed waits are not moved yet), and
`Crash*` and `HangWatchdog.*` run under TSan with the rest. The GPU suites run with
`halt_on_error=1` too: `tsan.supp` has Mesa's `swrast_dri.so`, whose reports are about its own mutexes. A budget
in wall clock is printed under a sanitizer, never enforced there: `perfSlack()`, or the build's
`JASPERENGINE_SANITIZE_*` where a test's own limit is unconditional.

### Warnings

`jasperengine_target_warnings()` (`engine/cmake/JasperWarnings.cmake`) puts
`-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare`
(MSVC: `/W3 /wd4244 /wd4267 /wd4305`) on `jasperengine`, the editor executables, the project
executable and the test targets; `JASPERENGINE_WERROR` adds `-Werror -Wno-error=deprecated-declarations`
(MSVC `/WX`). Vendored libraries keep `-w`/`/w` and `SYSTEM` includes. `tools/ci/warning-summary.py` counts
a build log's warnings per target.

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
file scope.

### Build speed

`docs/specs/S33-build-speed.md` has the measurements behind these and the tools to repeat them:
`tools/perf/build-bench.sh <label> <scenario>` (in the build image: clean, warm-cache, no-op and
edit rebuilds of one tree, with the host's load recorded) and `tools/perf/build_stats.py` (where a
`.ninja_log`'s time goes, header fan-out from `ninja -t deps`, header parse costs, `-ftime-report`).

- **Precompiled headers** (`JASPERENGINE_PCH`): `engine/cmake/JasperPch.h` (standard headers) for
  the engine, test and project targets, `editor/EditorPch.h` (plus `json.hpp` and yaml-cpp) for the
  editor's. Only our own C++ sources get them; a source with compile options of its own
  (`-mavx2`, `-ffp-contract=off`, ...) and the vendored sources compiled inside our targets do not.
  Nothing of ours goes in a precompiled header: it would make every unit depend on it. On in the
  editor tree and in `tools/build.sh`'s Linux development and test trees; off for exports, web,
  Android and players, which therefore check that every source includes what it uses. Not on MSVC
  (sources define `_USE_MATH_DEFINES` before `<cmath>`) or with the Xcode generator.
- **ccache** needs `sloppiness = pch_defines,time_macros` to cache a unit that uses a precompiled
  header; `tools/build.sh` sets it in the Linux images (no Linux unit uses `__DATE__`/`__TIME__`).
  `base_dir` is not set: it would hand the compiler relative paths and change `__FILE__`.
- **Linker** (`JASPERENGINE_LINKER=AUTO`): lld (the Linux image installs it) for Debug and
  RelWithDebInfo builds that ship nothing, where GNU ld spent 20-53 s per link; Release builds,
  the editor release and every shipped game keep the toolchain's linker.
- **Editor objects once**: `jasperengine-editor-objects` holds the editor's sources but `main.cpp`;
  the editor, its Windows command-line twin and the editor test targets link it.
- **Vendored libraries**: Jolt is unity-built where nothing ships (`JASPERENGINE_VENDORED_UNITY`);
  the others build source by source, and ccache serves them to every tree of a configuration.
- **Header fan-out** (S72): `JASPERENGINE_EXPLICIT_COMPONENT_INCLUDES`, defined on the engine's and
  the editor's targets (and the editor's tests, through the editor's definitions), turns off the
  includes `registry/EntityRegistry.h` and `object/GameObject.h` keep for code
  outside the engine: a source there includes each component and object it uses
  (`component/Transform.h`, `object/Mesh.h`), and a header that only names them takes
  `ecs/ComponentFwd.h` or a forward declaration. A game's code, the exported template and the core
  tests build without it and see everything as before. `tools/check_fanout.py` (run by
  `tools/check-changed`; `--table` prints the numbers) fails when a component header rebuilds more
  than 150 editor units, or the median one more than 80, from the tree's `ninja -t deps`.
  `tools/check_headers.py` compiles each header alone, without the precompiled header; `tools/check-changed`
  runs it on the headers a change edits. "Incomplete type" in a new source of `editor/` or `engine/core/`
  means an include this file used to get by accident: add it there, not to a hub.

- **C++-script games** (S30's Compile builds) build in a compile tree per kit in the user cache
  (`<cache>/jasperengine/compile-builds/<key>/`), shared by every project of that kit: the engine
  is compiled once and a game compiles its own units. A project's runtime settings (window,
  logging, crash options) are definitions of its executable only and reach the engine through
  `PlatformConfig`, so changing them recompiles a few units.

## No CI

Nothing runs on a push: a check counts only when someone ran it. `tools/ci/nightly-smoke.sh` is the
smoke test of every fixture project, and `tools/ci/check-release-tag.sh` a release tag's check.

Third-party notices are credits, not a gate: `tools/ci/check-notices.py` stages whatever licence
files and `NOTICE.md` rows exist into the editor install and exported games; CI does not fail on a
library without them.

The Chrome for Testing headless shell in `docker/web.Dockerfile` and `uv` in
`docker/linux.Dockerfile` are pinned by version and sha256 and bumped by hand, as are the MCP
harness's packages (`tools/mcp/requirements.txt`).

## Known gaps

What the suites do not cover today, and what closing each gap would take. Each needs money, a
device or a decision; recorded by BF23 (`docs/specs/BF23-authoring-audit-fixes.md`, section 4).

| Gap | Evidence | What it would take |
|---|---|---|
| No native compile check for generated HLSL and MSL | There is no CI, and `tools/check-changed` has no `fxc`, `dxc` or `metal` step | A check someone runs on a Windows and a macOS machine |
| Nothing runs on Windows or macOS unless a person does | `AGENTS.md`: there is no CI | A machine for each, or accepting it |
| Particle goldens run on glcore and Vulkan only | `docs/specs/S20-particles.md`, "Goldens" under Deviations | Devices for Metal, D3D11 and GLES3 |
| Mobile particle performance rows are unmeasured | `docs/perf/particles.md`, "Not measured here" | A person with reference phones |
| S20's "Web: est. X ms" hint was never built | `docs/specs/S20-particles.md` (Risks), no match in `editor/window/InspectorParticles.cpp` | Delivered by S91's `cost` table |
| The agent guide has 451 bytes free above its headroom, and S35, S90 and S91 all want some | `tool_budget.py --check`, measured 2026-11-04: 6,241 of 8,192 bytes, 1,951 left, 1,500 required | The second to land moves its own detail out, per `docs/mcp/conventions.md`. Measure with `tool_budget.py --groups all` |

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
25 %. The option stays (so new sources stay unity-safe),
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

### BF9 (compiled and prebuilt game builds in the MCP suite)

The full suite (`MCP_SUITE=all JASPER_SLOW_BUILD=1 JASPER_SLOW_TESTS=1 tools/build.sh mcp-tests
-j 2`) on server D (16 cores, the container capped at 8 CPUs, the host shared with other runs).

| Run | Runner wall | Test time | Result |
|---|---|---|---|
| before (S30 0ece8c33f) | 9,427 s (2.62 h) | 5.16 h | 1045 passed, 9 failed: S29's 7 `test_pose_runtime`, `test_build.py::test_desktop_build_and_run` (S30: the compiled game's job had no `executable`), `test_data_safety.py::test_crash_recovery` (once) |
| after, first (aa400ead2; both runtimes built cold, 3,627 s, host load 35 to 43) | 10,001 s (2.78 h) | 5.46 h | 1046 passed, 8 failed (the same, without `test_crash_recovery`) |
| after (e910cc237; runtimes rebuilt with ccache, 557 and 913 s, at nice 15; longest files first) | 8,161 s (2.27 h) | 4.47 h | 1047 passed, 7 failed (S29's `test_pose_runtime` only) |
| after the oe-port merge (dc6eb4f2d: S29's fix, S32, SYNC#3; tested at b95489ae6 with TOOLS.md regenerated) | 7,705 s (2.14 h) | 4.22 h | 1107 passed, 3 failed: `test_reflection.py::test_golden_resave` and `test_codegen_golden` (S32's goldens want the 2D scene's points transparent; `feat/s30-shipping` alone fails them the same way on B, without SYNC#3 or BF9), `test_vat_inspector.py::test_move_and_delete_keep_the_pair` (builds not done in 120 s on the loaded host; it passes on B) |

The files that build games went from 8,891 s of test time to 4,667 s (4,394 s after the merge). Per file, before and after:

| File | Before | After |
|---|---|---|
| `test_build.py` | 4,971 s | 2,788 s |
| `test_html_ui_video_export.py` | 1,272 s | 33 s |
| `test_call_cli.py` | 950 s | 17 s |
| `test_html_ui_export.py` | 813 s | 23 s |
| `test_html_ui_effects_export.py` | 877 s | 1,799 s |

Before, `test_build.py` stopped right after its compile, on the missing executable. After, it
also runs the compiled player and compiles the broken source. `test_html_ui_effects_export.py`
is still a compile; it took longer under that run's load. The files that build nothing ran 1.18
times slower in the last run than before (the host's load). At the earlier run's load the
suite's test time comes to about 3.8 h, about 1.9 h of wall time at `-j 2`.

What is left is not builds. Two compiles remain (about 1.3 h of test time, see the table above
for why), and the software-rendered golden and shader tests make up the rest
(`test_materials_pbr`, `test_html_ui_examples`, `test_shader_library`, `test_post_fx_*`,
`test_reflection`, `test_shadows`).

The compiled tests then lost their fixed 3 and 2 build jobs. They build with the container's CPU
allowance (`harness.build_jobs()`: `JOBS`, else the `--cpus` quota from cgroup `cpu.max`, 8 on D),
because Docker's cap is what shares the host. Re-run alone on D, at `-j 2` and load 9 to 23
(165c2b31e): `test_build.py` 2,675 s to 1,515 s (its compile 1,216 s),
`test_html_ui_effects_export.py` 1,638 s to 1,139 s, `test_busy.py` unchanged; 17 passed.

The compile path's missing executable is fixed: the Compile fallback holds its finish until the
content manifest and the executable are set. S30 made the same fix (9d854d51e, `holdFinish`),
which this branch now uses.

### BF16 (test speed, tools/check-changed)

The baseline is `int-mcp-a6701ba45` on A: 5.2 h of tests at `-j 2` in an 8-CPU container (10.3
test-hours, A at load 30 to 52), after 2.2 h of builds.

Where the 2.2 h went: S34's merge changed flags and widely included headers on a server that had not
compiled them, so the editor (2,238 steps), the sample project (1,646, a 19-minute LTO link, 976 s
on A at load 15 to 24) and the mesh-raycast web build (1,482) all missed. ccache across worktrees
already works: every worktree is `/src` in the image, so `base_dir` is not needed. An editor build of
oe-port in a new worktree: 2,144 direct hits of 2,150 compiles, 158 s.

What the slow tests spent ("now": the coverage run below, or the warm run of the files that build games):

| Test | Before | Now | What it was |
|---|---|---|---|
| `test_html_ui_effects_export::test_desktop_player_draws_the_effects` | 3,977 s | 302 s | a compile without ccache and an LTO link; ccache 1,237 hits, 8 misses, no LTO |
| `test_build::test_desktop_build_and_run` | 2,476 s | 929 s | a compile without ccache; now 601 hits, 8 misses; keeps LTO (the suite's LTO check) |
| `test_hub::test_create_from_templates` | 1,542 s | 177 s | a compile at 3 jobs; now 593 hits, 6 misses, no LTO |
| `test_materials_pbr::test_khronos_models` | 2,310 s | 13 tests, 19 to 491 s | polling every mesh, each call a frame; now a test per model, in chunks of their own |
| `test_shader_library::test_cli_library_matrix_all_backends` | 1,639 s | 249 s | one process compiling one variant at a time; now a process per backend |
| `test_fluid_inspector::test_preview_bar_on_a_play_created_fluid` | 1,815 s | 642 s | 3 to 5 s frames: a software-rendered fluid in the Inspector |

The seven files that build games, warm, `-j 4` in an 8-CPU container on A: 1,038 s, 42 passed, both
runtimes reused from the shared cache. The same files with a new runtime key: 5,284 s, of which 3,744
and 4,223 s of runtime builds at nice 15 (172 and 254 misses). The prebuild does not wait for its
turn: when only files that need a runtime are left, a worker builds the second one at once.

A runtime build with an empty ccache, no LTO, both at once at nice 15 in an 8-CPU container: 2,414 s
(Development) and 2,388 s (release), against 6,624 s and 4,816 s (with LTO) in the baseline.

The coverage run (the suite without the slow builds, an instrumented editor, `-j 4` in an 8-CPU
container, H at load 30 to 37): 4,822 s, 5.2 test-hours, longest test 642 s, 1,181 passed, 1 failed (below),
7 skipped by design (the slow builds; the web latency gate).

Editors on the RTX 3080 Ti: the 34 lane files, 7,259 s of test time on llvmpipe under load, take
1,223 s, two at a time. `test_fluid_inspector.py`'s 2,296 s took 53 s there, and one absolute dye
threshold keeps it on llvmpipe.

`tools/check-changed` on past streams, each its own tree and diff, with this tool (`--root`), in 8-CPU
containers. The trees predate the caps and the caches, so their MCP times are not this branch's.

| Stream | Changed | Selected | Result |
|---|---|---|---|
| fix/mac-test-engine-dir | 1 header | 10 of 188 MCP files, 16 tests; 19 editor suites | passed: units 25 s, MCP 164 s (of which the web build 148 s) |
| BF12 fix/screenshot-texture-wait | 15 files | 160 files, 1,055 tests; 113 core and 211 editor suites | units passed; MCP 8,986 s, 1,048 passed, 7 failed: frame or preview timeouts and a 4.5 s hang report at high load, no Mesa thread cap in that tree. Its red test (`test_screenshot_waits.py`) and `GltfTexturePixels` were selected and passed |
| BF13 fix/bf13-editor-dataloss | 63 files | 187 files, 1,174 tests; 72 core and 211 editor suites | MCP 8,055 s, 1,173 passed, 1 failed (a 3.7 s hang report at load 40 to 54). The tests that failed in its logs (`test_camera_texture_cost.py`) were selected and passed. `NavAgentTest` failed: below |
| S34 feat/s34-player-size (dry run) | 75 files | everything: the root `CMakeLists.txt` | not run |

So a narrow change selects few tests and a stream that touches `create_entity`, `screenshot` or the
frame selects most of the suite (of 1,189 tests, the CI suite alone is 1,131, so its fallback is
nearly the full suite too).

Found on the way:

- `test_reflection.py::test_codegen_golden` is flaky on oe-port, on llvmpipe and on the RTX:
  `RigidbodyComponent::solidBoxDensity` has no initialiser, so the generated C++ prints garbage
  (`-95312802931886636396528534812354412544.000000f`). BF14 gives it one.
- The same test failed once on the RTX on `clouds.weatherMap.setPath(...)`: whether a generated scene
  sets a texture's path depends on when its load finished. Fixed in `Factory::formatTexture` (BF20).
- Nine files failed on the RTX (BF20 has them all on the lane now):
  - Pipelines are made off the render thread on a GPU and at once on llvmpipe, so whatever draws
    once and keeps the result must wait for them. A stateful UI effect ran its passes without the
    ones whose pipeline was not ready (`test_html_ui_effects.py`'s fog, `test_fluid_effect.py`'s
    burst): the passes now start over until each has its pipeline. A pose sheet read back a target
    the model had not been drawn into (`test_pose_render.py`): it now waits for `getWaitingDrawCount`
    to stand still. `MeshSystem::loadGLTF` left a reload's `vertexCount`s to `RenderSystem::loadMesh`,
    seconds later on a GPU, and a save meanwhile wrote those of the reversed slots
    (`test_pose_window.py`).
  - Tests: `test_fluid_inspector.py`'s dye amount follows how many frames a drag takes, and
    `test_place_with_physics.py` raced a job against a CPU and frames faster than llvmpipe's.
  - Not reproduced on af184d8f1: `test_upgrade_from_main.py`'s rewritten `Level1.scene` and
    `test_lattice_bake.py`'s 1.9%; BF16's runs predate BF15's late image binds.
- `tools/build.sh linux`'s LTO link prints 11 `-Wodr` warnings about `spv::` enums, the two vendored
  copies of `spirv.hpp` (glslang, spirv-cross), since S34. No warning from our own targets.
- `NavAgentTest.CharacterControllerDriver` depended on the order of the tests: `NavSceneTest::step()` ran
  outside the fixed phase, where a CharacterController moves by `Engine::getDeltatime()`, the last frame
  of whatever earlier test ran one. A selection (with `AnimatorGpu.ThreeControllerPosesAndARetargetedWalk`)
  failed it every time, alone it passed. The fixture now steps in the fixed phase. A filtered run can
  show such a test up; a full run's order hides it.
