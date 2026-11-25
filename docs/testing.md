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
| Foliage (T5) | placement, cull, tree store, rules (`Foliage*`, `TreeStore*`: core CPU), the system (`FoliageSystemCore`, `FoliageTreesCore`, `FoliageRuntime`), the draws (`FoliageGpu`: goldens in `engine/tests/core/render/golden/foliage_*`), `je-bench-foliage`, `tools/mcp/tests/test_foliage*.py`; `docs/specs/checklists/T5.md` by hand | `UNIT_SUITE=core UNIT_TEST_ARGS=--gtest_filter='Foliage*:TreeStore*' tools/build.sh unit-tests`; `MCP_SUITE=all tools/build.sh mcp-tests -k foliage -j 2` | `docs/perf/foliage.md` |
| Terrain budgets (T1) | the stamps, whole-map ops, queries, refit and runtime edits (`je-bench-terrain`, gated by `tools/perf/check_bench.py`), the delta step (`TerrainDeltaBench`: editor, budgets under `JASPER_TEST_PERF_SLACK`), the player's growth (`tools/perf/terrain_size.py`); pen tablets by hand, `docs/specs/checklists/T1.md` | `cmake --build <core-tests tree> --target je-bench-terrain`, run it with `--json`, then `check_bench.py`; `UNIT_SUITE=editor UNIT_TEST_ARGS=--gtest_filter='TerrainDeltaBench.*' tools/build.sh unit-tests` | `docs/perf/terrain.md` |
| Animator benchmark on WebAssembly | `je-bench-anim` under node in the web image, with and without `-msimd128` (S77 R11); `tools/perf/check_bench.py` gates the owner's budgets as `je-bench-anim-wasm` | `tools/perf/bench-anim-wasm.sh`, results read in `docs/perf/animator.md` | `build/test-results/anim-wasm/` |
| Smoke test | export, build and run a project, then open it in the editor; screenshots | `tools/smoke-test.sh tools/fixtures/post-processing` | `build/smoke/` |
| Upgrade from origin/main | a game an origin/main editor made (`tools/test-projects/upgrade-from-main`) opened, migrated, played, exported for Linux and the web in this editor, against the old editor's transcripts ([upgrading-from-main.md](upgrading-from-main.md)) | `tools/build.sh linux`, then `tools/ci/upgrade_from_main/check.sh` (the editor part also in the MCP suite: `test_upgrade_from_main.py`) | `build/upgrade-from-main/check/report.md` |
| Web gate | the web build in headless Chrome (WebGL2 on SwiftShader) | `tools/build.sh web`, then `tools/build.sh web-gate` | `build/test-results/web/` |
| Android render gate | an APK on the Android emulator: logcat, GLES errors, and the screen against goldens (a lit 3D scene, 2D/UI, HTML UI) | `ANDROID_PROJECT=render-gate ANDROID_ABIS=x86_64 tools/build.sh android`, then `tools/android-emulator-test.sh` (below; KVM servers A and C) | `build/android-emulator/<scene>/` |
| Crash diagnostics | the crash handler's report folders, sessions, hook and ring, the hang watchdog (`HangWatchdog.*`, `Crash*` in `jasperengine-core-tests`, which start `jasperengine-crash-test`, `engine/tests/crash/CrashTestMain.cpp`), the symbolication tool, the web crash hooks | `tools/build.sh unit-tests`; `python3 -I tools/crash/test_symbolicate.py <jasperengine-crash-test>`; `WEB_PROJECT=crash-diagnostics tools/build.sh web`, then `GATE=crash-diagnostics tools/build.sh web-gate` | as above |
| Android packaging | what Build makes for Android, run for real: APK and App Bundle from the prebuilt player, the bundletool download (real URL), the debug and release keys, wrong passwords, `publish prepare`, then both installed on the emulator and their screens compared with the golden | `tools/ci/android_packaging.sh` (needs `tools/build.sh linux`, the Android player; KVM: server A) | `build/android-packaging/` |
| iOS packaging | what Build makes for iOS on a Mac: the Xcode project through `xcodebuild`, installed and launched on a simulator device of its own (no window), a device build as far as signing, the IPA path without a developer account, `publish prepare` | `tools/ci/ios_packaging.sh` (needs the editor on a Mac with Xcode, `tools/players/ios_player.sh`; `IOS_PACKAGING_GOLDEN=1` compares the screen with `tools/fixtures/unified-lighting/golden/ios/screen.png`, from an iPhone 17 simulator) | `build/ios-packaging/` |
| Tool checks | the shader lint, the CI scripts' own tests, the presets, the HTML UI golden ledger, the exported projects' `jasperengine.py`, C++ scripts linking Box2D and Jolt (run it on Windows and macOS by hand: nothing else checks those), the Android Gradle files | `python3 tools/ci/test_check_shader_lod.py` (with `python3 tools/check_shader_lod.py`: no implicit derivatives in a shader loop with a per-fragment exit or branch), `python3 tools/ci/test_gtest_gate.py`, `python3 -I tools/ci/test_pr_evidence.py`, `python3 tools/ci/test_smoke_jit_hangs.py`, `python3 tools/ci/test_android_screen_check.py`, `python3 tools/ci/test_check_notices.py`, `python3 tools/ci/test_jasperengine_py.py`, `python3 tools/ci/check-script-physics-link.py <editor>`, `python3 tools/ci/test_check_gradle_warnings.py` (and `tools/build.sh android` runs `check_gradle_warnings.py` on its Gradle log: a deprecation in our Gradle files fails the build), `python3 tools/ci/test_check_golden_ledger.py`, `python3 tools/htmlui/check_golden_ledger.py`, `python3 tools/check-editor-modals.py` (`--self-test`), `tools/build.sh exec linux tools/ci/check-presets.sh`, `python3 tools/procmat/test_import_mm_nodes.py` (S90's node importer: licences, the deny-list, what is data) | stdout |

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
GRAPHIC_BACKEND=gles3 UNIT_SUITE=core tools/build.sh unit-tests    # GPU suites in an OpenGL ES 3.2 context (llvmpipe, linux-backends image)
WERROR=1 tools/build.sh unit-tests
RUN=0 tools/build.sh unit-tests                             # build only
UNIT_SHARD=1/2 UNIT_SUITE=editor tools/build.sh unit-tests  # shard 1 of 2 of the selected tests
UNIT_BUILD_PART=1/2 UNIT_SUITE=editor tools/build.sh unit-tests  # compile part 1 of 2 of the objects, then stop
```

`UNIT_SHARD=K/N` runs whole suites, balanced by test count, out of what the filter selects (the filter
minus the other shards' suites; single tests when there are fewer suites than shards, all of them in
every shard when there are fewer tests), so no shard runs nothing; an unfiltered shard must reach its
share of the counts above. `UNIT_BUILD_PART=K/N` compiles every Nth object of the binary's compile
steps (`ninja -t commands`, sorted), the same part in every tree of one configure, through
`unit-part.ninja`: `build.ninja` without the libraries and executables CMake makes compiles and code
generators wait for (order-only), so a part links nothing. CI's editor parts share them through ccache
(below, "CI"). Both are `tools/ci/unit_split.py`
(`python3 -I tools/ci/test_unit_split.py`).

The binaries also register with CTest (`ctest --preset linux-ci` in an IDE build), labelled
`unit`, `gpu` and `slow` by suite name.

Picking (S26): `JASPER_PICK_SIMD=scalar|avx2|neon` forces the mesh BVH's slab test path; `MeshPose.MatchesGpuSkinning` is a GPU suite (the
CPU pose against the skinned shaders, glcore and Vulkan). `JASPER_PICK_BUILD_DELAY_MS=<ms>` holds
every background mesh BVH build pending that long (the wait runs on a blocking thread, so it holds no
Background gate slot other loads queue for): the MCP tests use it to see the Scene view's
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
`JASPER_TEST_TRANSITION_TWIN_SKEW=<coverage>` shifts the CPU evaluation a transition capture's `transition_edges` compares the frames with, for the lint's red path (`tools/mcp/tests/test_contact_sheet.py`).
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
  skips there. See `docs/mcp/README.md` for writing MCP tests. A project the harness creates
  (`Editor(create=True)`) keeps the flat layout the tests are written against, its assets and
  scripts at its root: the harness sets `JASPER_TEST_FLAT_NEW_PROJECT=1` unless the test passes
  `env={"JASPER_TEST_FLAT_NEW_PROJECT": "0"}`, which gives a new project's real layout (`assets/`,
  `scripts/`, `test_asset_stripping.py`).
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
- `SMOKE_METHOD=build SMOKE_PLAY=1 tools/smoke-test.sh tools/fixtures/title3d-fixed`: a Compile build with only the engine modules the project needs (`jasperengine-editor build --method compile`): a fixed-text Realtime 3D title without the mesher, checked against the editor's frame.
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
- The other test projects with a gate: `atmosphere` (S12), `post-fx` (S13), `pcss` (PCSS's two views
  of the shadow atlas in one WebGL2 draw, the Ultra tier's contact hardening) and `terrain`, a
  terrain displaced by a 16-bit heightmap, which WebGL2 and GLES3 cannot sample as R16 (the
  renderer uploads it as R32F or R16F there). `terrain-edit` runs the op sequences of
  `TerrainGolden.*` in WebAssembly and fails when their digest differs from the one the native
  core suite pins (T1 AC5). `foliage` hashes T5's placement fixture the same way, then fills a new
  ring after a camera jump with one thread, within 4 ms a frame at the p99 (T5 AC1, AC4).
- `GATE=web-keys-audio` (after `WEB_PROJECT=web-keys-audio tools/build.sh web`) runs Chrome with the
  desktop autoplay policy (`chrome_flags` in the baseline): the page's audio is suspended until the
  first key press (`keys`: trusted CDP key events, `eval_before_input` before them, `eval` after),
  and Meta, ContextMenu and events without a code reach the game as their engine keys.
- `GATE=pixel-perfect` (after
  `WEB_PROJECT=pixel-perfect tools/build.sh web`) runs at a device scale factor of 2 and checks that
  a Pixel Perfect Camera sizes the canvas backing store at its CSS size times `devicePixelRatio` and
  draws it pixelated (S41 AC13).

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
  | `test_lifecycle_export.py::test_play_and_export_log_the_same_lifecycle` | a C++ script's lifecycle in an exported player: C++ scripts ship only in a compiled game |

- **(b) Prebuilt** (`method="prebuilt"`, and it checks `method_used` and the `player_id` it
  used). The test needs a real game that runs, not the compile:
  `test_build.py::test_development_build_crash_report` (the Development runtime crashes on
  purpose and writes its report), `test_html_ui_export.py::test_desktop_build_runs_the_document`,
  `test_html_ui_video_export.py::test_desktop_build_plays_the_video`,
  `test_navigation_export.py::test_desktop_export_ships_navmesh` (a baked `.navmesh` in the
  build's `content.jpak`, an agent of the headless player reaching its destination from Lua, and
  the preflight's stale-navmesh warning after static geometry moved; S19 AC27, S74 AC29) and
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
  `build/mcp-compile-home/<file>/`, emptied as the file's first editor starts (not as the file loads:
  any run's `-k` listing loads every file), and ccache as CMake's compiler
  launcher. Each run still configures and compiles a fresh tree and links; an object comes from
  ccache only when its preprocessed source, flags and directory are those it was made from. They
  link without LTO (`JASPERENGINE_RELEASE_LTO=OFF` in the editor's environment, which an exported
  game's first configure takes as the option's default) except `test_build.py`'s (`lto=True`).

A new test that needs a game that runs builds it prebuilt. One that needs what only a compiled
game has (C++ scripts, `ProjectBuild.cmake`, capacities above the player's, native plugins a player cannot carry, no runtime
shader compiler, compiler output) compiles, and joins the table above with its reason.

### The noise golden tool

`jasperengine-noise-golden` (`tools/noise/golden`: noise, graph and the table, no engine) is the self-check of
T2's one golden table: `cmake -S tools/noise/golden -B build/noise-golden -G Ninja -DCMAKE_BUILD_TYPE=Release`,
then `build/noise-golden/jasperengine-noise-golden --all-backends --report -`. It prints
`noise-golden <target> <backend> <cases> pass|FAIL table=<hash>`, exits 1 on any mismatch, and `--self-test` must
exit 1. `tools/build.sh exec android tools/noise/cross_check.sh` runs it on the four Android ABIs (ARM under
qemu-user); `--red-contract` must fail. `WEB_PROJECT=noise tools/build.sh web`, then `GATE=noise tools/build.sh web-gate`,
runs the same table on the web, also with `WASM_SIMD=0`. `python3 tools/noise/check_kernels.py [--self-test]` is
the lint for this area, which `tools/check-changed` does not run. A difference is a kernel bug, never a new
table; record each platform's report in `docs/perf/noise.md`.

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
(`ui2d`: a textured Image, Polygons, a Text), an HTML UI document (`htmlui`), the JasperArcade port's
scaled list of filtered rows clipped by its viewport (`htmlui-u27`, S92 R11) and Title Art
Renderers (`titleart`, S81; run it also with `--setprop debug.jasperengine.titleart_rgba8=1` against
the same golden), goldens in its `golden/`:

```sh
ANDROID_PROJECT=render-gate ANDROID_ABIS=x86_64 tools/build.sh android
tools/android-emulator-test.sh build/android-render-gate/app-debug.apk \
    --expect-file tools/test-projects/render-gate/android-expect.txt \
    --scene lit3d --scene ui2d --scene htmlui --scene htmlui-u27 --scene titleart --golden tools/test-projects/render-gate/golden
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

## Gates: per commit, then CI

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
- **CI owns the full suite** (below, "CI"): a PR runs the lanes its diff needs, the merge queue
  every lane that can block the merge, the advisory run a push to `main` dispatches the advisory test
  lanes the queue left out, the nightly the whole MCP CI suite;
  `gh workflow run ci.yml --ref <branch> -f full=true` runs everything on a branch. check-changed is
  a selection and says so in every summary. The MCP files outside the CI suite (`MCP_SUITE=all`)
  run only where someone runs them.
- **Lints check-changed does not run**, by area (`AGENTS.md`, Test): `tools/check_raw_writes.py` and
  `tools/check_stream_keys.py` (file writes, decoders), `tools/check_text_encoding.py` (code-page
  conversions, Win32 `-A` calls, Apple string encodings, vswhere; `--self-test` is its red control),
  `tools/ci/check-hardcoded-shortcuts.py`, `tools/ci/check-notices.py`.

### `tools/check-changed`

```sh
tools/check-changed                           # build and test what the change touches
tools/check-changed --dry-run                 # the plan only
tools/check-changed --base main               # another base (default: the merge base with oe-port, else main, origin/main)
tools/check-changed --no-mcp                  # builds and unit tests only, said so in the summary and not cached
tools/build.sh exec linux tools/check-changed # inside the Linux image (no web or Android build there)
```

- Tool tests: a changed `tools/<dir>/<x>.py` runs `tools/<dir>/test_<x>.py` beside it (and a changed
  test itself), unless it takes arguments only its lane has (`SELF_TESTS_ELSEWHERE`); `SELF_TESTS`
  names the rest. CI's lint lane runs every tool test that needs no build (`tools/ci/lint.sh`).
- Lints with triggers: a changed MCP test file runs `tools/ci/check-mcp-ci-suite.py`; a changed
  engine source (not `engine/libs`, `engine/tests`) runs `tools/check_event_clears.py` (every
  `FunctionSubscribe` member is in its holder's `forEachEvent`, every component holder in
  `ComponentEvents.h`, every other holder in the lint's `WALKS` or `TRANSIENT`, and every static
  event in Engine's clears, so the clears at Stop reach it; `Ease` members count; `tools/build.sh
  linux` runs it too).
- The change: committed since the base, staged, unstaged and untracked files. A checkout without
  the stream's history reads its diff from `build/.rb-changes.json` (with every file's sha256: a
  diff that does not match the files is refused).
- Unit tests: the gtest suites (`TEST`, `TEST_F`, `TEST_P`, typed) of the test units that see the
  change, in `jasperengine-core-tests` and `jasperengine-editor-tests` (`--gtest_filter`, through
  `tools/build.sh unit-tests` and its no-silent-green gate); a changed test file always runs; a
  CMake change, or test trees not built yet, run them all. A changed source the tree links but no test
  unit includes (a `.cpp` whose functions only other sources call) runs the units that include its
  nearest consumers: the sources that include it, through their own headers, a step at a time until a
  test unit includes one (`nearest_consumers`; on `main`'s dumps of 2026-11-20 it takes the core
  tree's sources that selected nothing from 620 of 2,175 to 240, the editor tree's from 1,659 of
  4,333 to 600; what is left registers itself, as the Lua bindings do, and no include reaches it).
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
- Navigation's runtime and the engine's CMake (`NAVIGATION_OFF_PATHS`: `engine/core/navigation/`,
  `engine/libs/recastnavigation/`, `NavigationSystem.*`, `engine/core/object/navigation/`,
  `engine/core/component/Nav*`, `engine/CMakeLists.txt`, the navigation tests and the script) also
  plan `navigation-off`: `tools/check-navigation-off.sh` in the Linux image (below, "Navigation
  OFF").
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
each with its own worker process and editor (`run_tests.py --split-seconds N`). `test_html_ui_examples.py` (each example's golden), the
Khronos models of `test_materials_pbr.py` (`test_khronos_<Model>`) and the parts of the Title Art
library's gate (`test_title_library.py`, `test_gate_<k>_of_8`) declare it.

### Setup cost

The runner prints `== setup` before its summary and writes `setup` into `--json`: each chunk's wall
time and its tests' time, and each editor's `scratch` (its directory and project copy), `start` (to
the tool list), `first_call` (the first tool call, which waits for the project to load), `close`, and
`shared` for the file's editor (the others are started by its tests). Measured:

- This split's first CI run, 44838269359 (the CI suite in 7 shards: 438 chunks, 2,649 tests): 8.79 h
  of chunk time, 8.59 h of it inside the tests. 693 editors, 409 of them the files' and 284 started by
  tests in 56 files (`test_saved_output_determinism.py` 36, `test_post_fx_settings.py` 35,
  `test_reflection.py` 34, `test_project_audio_defaults.py` 29); medians: start 0.37 s, first call
  1.93 s, close 0.58 s. Outside the tests a median 1.3 s a chunk (the worker's start, 0.5 s of it
  importing the harness, and the file's editor close). Setup at most 0.77 h (8.7%): the files' editors
  0.36 h, the tests' own 0.31 h.
- The load, in the Linux image on llvmpipe at 2 to 4 CPUs: 1.5 s, the same for `create` and a
  `tools/fixtures` copy; 0.6 s of it is the engine API the editor syncs into
  `.jasperengine/engine-api` (3,554 files).
- Fixture copies take milliseconds (23 s for the whole run): of the CI files, 343 start on `create`, 21
  on `pose-animation`, 15 on `html-ui`, none on `build/fixture-unity`; the largest `tools/fixtures`
  project is `reflection-parity` (14 MB, 2 files), the next 1.5 MB. A file's first test takes as long
  whatever its fixture (CI median: `create` 6.0 s, `pose-animation` 4.5 s, `html-ui` 6.9 s).

So each file keeps a worker process and an editor of its own: a reset between files in one editor
(`create_project` with `unsaved: "discard"`) takes 1.6 s against 2.3 s for a new editor's start,
load and close, under 1% of the suite, and would hand the editor's process state (settings, layout,
Lua, tool groups, console) to the next file. A pre-synced engine API cannot help `create`
(`--create` takes an empty directory only). For shard planning: a chunk costs about 1.3 s outside
its tests plus 2 to 3 s for its editor inside its first test, not the 20 s of `shard.py`'s
`CHUNK_OVERHEAD`.

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
| editor code that needs a GPU device (S90's evaluator, `editor/procmat/`) | `engine/tests/core/procmat/*GpuTest.cpp`, the editor sources listed in `engine/tests/core/CMakeLists.txt` | `jasperengine-core-tests` (only that tree has a headless device) |
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
- A GPU test that takes more than 5 minutes under ASan goes in a suite ending in `GpuSlow` (a fixture
  of its own, `class TitleGenGpuSlow : public TitleGenGpu {};`): the PR ASan lane leaves `*GpuSlow*` to
  the nightly's ASan job, and every other lane runs it as a `*Gpu*` suite (`-*Gpu*` leaves it out too).
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
  `ShadowBench`, S03's `AtomicFileBench` Document write), `SchedulerTest.Process10kUnder2ms` and S12's `AtmosphereNoise` bake budgets check
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

An HTML UI frame that records what the last drawn one did is not replayed (S92 R1). A test that
counts a replay's passes, draws or caches frame after frame turns that off for its scope
(`FrameReuseMode replayed(false)`, `TestRenderCommon.h`). `JASPER_HTMLUI_VERIFY_KEPT=1` makes the
golden harnesses draw a second frame and compare every kept frame with a fresh replay; a
difference fails the comparison and logs `[HTML UI] kept frame differs`:

```sh
JASPER_HTMLUI_VERIFY_KEPT=1 HTMLUI_TEST_ARGS='--gtest_filter=*Golden*:RenderP1*:RenderPrimitive.*:RenderClip.*:RenderFilter*' tools/build.sh htmlui-tests
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
| `JASPERENGINE_DEBUG_INFO` | `full` (a first configure: the environment's `JASPERENGINE_DEBUG_INFO`) | `lines`: line tables only wherever the tree has debug information (below) |
| `JASPERENGINE_VENDORED_UNITY` | ON, OFF for a shipped game | Jolt unity-built |

`engine/CMakeLists.txt` is also the top-level file of every exported game, so all of these default
to off there, and the exporter never sets them: a game configures and builds exactly as before.

### Navigation OFF: `tools/check-navigation-off.sh`

A project with Project Settings > Navigation > Include In Builds off exports with
`JASPERENGINE_NAVIGATION=OFF`: Recast/Detour is not compiled, every navmesh bake, query and agent
fails and logs once, and Nav Grids keep working. `NavStubTest` checks that, and it is compiled only
in such a build, which no other suite makes (S74 R28):

```sh
tools/build.sh exec linux tools/check-navigation-off.sh     # build/navigation-off, build/test-results/navigation-off.xml
```

It configures the standalone engine (`engine-linux-tests`, `-DJASPERENGINE_NAVIGATION=OFF`, warnings
not errors), builds `jasperengine-core-tests`, runs `NavStubTest.*:NavGridTest.*` through the
no-silent-green gate, and fails unless at least two `NavStubTest` tests passed (a tree that still
compiles navigation in runs none). The first run builds the whole engine (the switch is a public
define); later runs are incremental. `tools/check-changed` plans it for a navigation or engine CMake
change; nothing else runs it, CI included.

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
Clang), `-O1` in Debug (at `-O0` the editor test binary's instrumentation data alone passed the
2 GB that x86-64's small code model reaches, and it did not link), and defines
`JASPERENGINE_SANITIZE_ADDRESS`, `_UNDEFINED` and `_THREAD` so tests can relax time budgets.
`thread` with `address` is a configure error, as are `thread` and `undefined` on MSVC and any
sanitizer on Emscripten, Android or iOS. On MSVC, `address` switches to the DLL runtime, drops `/RTC1`,
links with `/INCREMENTAL:NO`, defines `_DISABLE_STL_ANNOTATION`, and copies
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

Werror is enforced with GCC in CI's `linux` and `unit` lanes (`WERROR=1`). It is never enforced on
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
file scope. Nor may a source hand the sources after it anything at file scope: a `using namespace`
of anything but `jasperengine` and `jasperengine::editor` (take the names it uses, or put the
tests in that namespace), a global alias such as `using Json = ...` (give it a name of the file's
own), or a macro (`#undef` it at the end). A name an engine header shares with an editor one
(`SelectionState`, `View`, `pixelart`, `test`) is spelled out where both are in sight. Each tree
CI builds compiles with `UNITY=1`: `tools/build.sh linux`, and `unit-tests` for both suites.

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
  (sources define `_USE_MATH_DEFINES` before `<cmath>`) or with the Xcode generator. CI turns them
  off everywhere (`JASPERENGINE_PCH=OFF` in `ci.yml`, which `tools/build.sh` and
  `tools/ci/macos.sh` pass to every tree; set it to reproduce a lane exactly).
- **ccache** needs `sloppiness = pch_defines,time_macros` to cache a unit that uses a precompiled
  header; `tools/build.sh` sets it in the Linux images (no Linux unit uses `__DATE__`/`__TIME__`).
  A fresh tree at the same path then hits every unit while the cache holds the header's own entry:
  a unit hashes the `.gch`, and two GCC builds of one header differ in their bytes, so once that
  entry is evicted every unit built on it misses (a cache cut to 58 % of a core-tests build gave
  15 % hits). Measured on the core-tests tree (6 CPUs, 1,900 units, host load 11-23): cold 1,403-1,575 s with
  precompiled headers, 1,460 s without; warm 126-141 s with, 90 s without. Hence CI's `OFF`. In the
  Android image ccache names the compiler by `--version`: the NDK is unpacked in each container, so
  its mtime changes every run.
  `base_dir` is not set: it would hand the compiler relative paths and change `__FILE__`.
- **Debug information** (`JASPERENGINE_DEBUG_INFO`, `engine/cmake/JasperDebugInfo.cmake`): `lines`
  turns every `-g` a tree gets (Debug and RelWithDebInfo, a shipped game's split symbols) into GCC's
  `-g1` or Clang's `-gline-tables-only` (Emscripten included); a Release tree without debug
  information gets none, and MSVC keeps `/Z7` (it has no such level). Backtraces, crash reports and
  sanitizer reports still name each frame's function, file and line (the logger's backtraces use the
  dynamic symbols, `dladdr`); a debugger sees no types or local variables. CI builds every tree with
  it (`JASPERENGINE_DEBUG_INFO=lines` in `ci.yml`; `tools/build.sh` passes `-DJASPERENGINE_DEBUG_INFO`
  to each tree it configures, `full` when unset, and the variable into the image, where the games the
  editor configures take it as their default; `tools/smoke-test.sh` passes it too), so the lanes'
  ccache entries fit the budget ("CI"). Nothing else sets it: local builds, `build-release.yml`'s
  editors and players, and the games users export keep full debug information. Set it to reproduce
  a lane exactly.
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

## CI

GitHub Actions (`.github/workflows/`) runs on every pull request to `main`, in the merge queue, on
every push to `main` and in the advisory run it dispatches, every night and for a release. Each step is a one-line call to a script in this
repository, in the same Docker images `tools/build.sh` builds, so every lane reproduces locally with
the command its report prints. The design is `docs/superpowers/specs/2026-11-12-ci-design.md`.

### Lanes

Jobs of `ci.yml`; a lane's command is also its repro.

| Lane | Runs |
|---|---|
| `changes` | `tools/ci/plan_lanes.py`: `tools/check-changed`'s plan for the diff, widened by CI's rules below; uploads it as the artifact `plan` |
| `lint` | the repository's and the workflows' lints |
| `linux` | `WERROR=1 tools/build.sh linux`; its editor install feeds `mcp` and `smoke` |
| `unit` (one job per test binary: `core`; `editor-1`, `editor-2`) | `UNIT_SUITE=<suite> UNIT_TEST_ARGS='--gtest_filter=<f>' WERROR=1 tools/build.sh unit-tests`, plus the scalar picking rerun (core); each editor part with `UNIT_SHARD=<k>/2`, after compiling half the objects (`UNIT_BUILD_PART=<k>/2`) and taking the other half from the other part (below) |
| `asan` (one job per test binary: `core`, `editor`) | `SANITIZE=address,undefined UNIT_SUITE=<suite> UNIT_TEST_ARGS='--gtest_filter=<f>-*GpuSlow*' tools/build.sh unit-tests`, the unit lane's filter (on a PR: below) without the `*GpuSlow*` suites (the nightly runs them under ASan); the editor job prints how much of x86-64's 2 GiB relocation reach its test binary uses (`tools/ci/reloc_reach.py`) |
| `htmlui` | `JASPER_HTMLUI_PERF_SLACK=3 tools/build.sh htmlui-tests` |
| `mcp` (one job per shard) | `tools/ci/mcp_shard.sh <k> build/ci-plan`: the CI suite's planned files, `-j 2`, without the fixture project |
| `smoke` | `SMOKE_METHOD=compile tools/smoke-test.sh tools/fixtures/post-processing`, the prebuilt `tools/fixtures/unified-lighting` and a project from each template |
| `web` | `tools/build.sh web && tools/build.sh web-gate` |
| `android` | `tools/build.sh android` (`ANDROID_ABIS=arm64-v8a` on a PR) |
| `windows` (parallel jobs: `windows-editor`, `windows-engine-glcore`, `-d3d11`, `-vulkan`, `windows-compile`) | `tools/ci/windows.sh <step>` on `windows-latest` with MSVC: the editor with its CPU unit tests (`TEST_SUITES=editor`, reported as `windows-editor-tests`, stopped after `TEST_TIMEOUT`) and the call CLI; the engine on each backend, OpenGL's with the core tests (`TEST_SUITES=core`), the D3D11 and Vulkan legs on a PR and in the queue only for their inputs (below); then a Compile build of a fixture with the editor job's install. Each job keeps its own ccache entry |
| `macos`, `ios` | `tools/ci/macos.sh <step>` on `macos-latest`: the editor (arm64), the engine, the CPU unit tests, a Compile build of a fixture; `XCODE_CAS=build/.cache/xcode-cas tools/ci/macos.sh xcode ios` (and `macos`) for the Xcode project, through Xcode's compilation cache |
| `report` | writes the PR comment (below) |
| `check` | the one required status, `Check` (`tools/ci/check_gate.py`): green only when every lane that had to run passed |

`Check` treats a skipped lane as passed only when `changes` said the lane was not needed. A test lane
that ran no test, an MCP shard that ran no file, a unit job (`unit-core`, `unit-editor-<k>`) or an
ASan job (`asan-core`, `asan-editor`) without a report or that ran no test, or a merge-queue or full
run that skipped a lane turns it red. The one skip the queue may make is of an advisory lane or test
run that `changes` planned off for being advisory (`lanes.json` `advisory_off`).

**The editor parts.** The editor tests' build is most of their lane (runs 43737549764 and 43773511699:
the build 110 and 137 min, the 5,268 tests 7 min). Each part (`tools/ci/plan_lanes.py` `UNIT_PARTS`)
compiles every other object into the restored ccache, uploads the files that added
(`unit-editor-<k>-ccache`, a day), and waits for the other part's (`tools/ci/unit_split.py wait`): no
longer than its own part took, not at all when that part has not started or ended without them, or
when its own part compiled under a tenth of its objects (a warm cache: the other part's hit too). It
then builds the binary from the cache, compiling whatever it did not get, and runs its shard. Only
the first part makes the deps dump on push; the second only compiles its objects there. The hits need a configure whose objects do not depend on a precompiled header built in
the job: a GCC `.gch` is not the same bytes twice, so a unit built on the other part's misses. A skipped MCP test fails its shard unless `tools/ci/mcp-allowed-skips.txt` lists it
with the reason and the lane where it does run; a new entry is reviewed like any change.
PR and merge-queue shards run without `JASPER_SLOW_BUILD`: the tests that build a real game (an hour
or more each, cold) skip there and run in the nightly, and the smoke lane builds and launches games
on every change a game's build reads.

**Advisory lanes during rollout.** `ci.yml`'s `ADVISORY_LANES` holds `unit asan htmlui mcp smoke`
until `main` is green through CI, and `windows-editor-tests` (the Windows editor job's test run,
reported apart from the job, until the suite passes on Windows): `Check` ignores their failures and
does not wait for them (its `needs` are `report`'s without them, which `tools/ci/test_check_gate.py`
checks), and the report, which waits for every lane, shows them first, marked advisory. Lint and the platform builds always block, the
Windows jobs' builds and the core tests' run included. Since an advisory lane cannot block a merge,
the merge queue does not run it (`tools/ci/plan_lanes.py --advisory "$ADVISORY_LANES"`): of `unit`
(every job of it), `asan`, `htmlui`, `mcp`, `smoke` and `windows-editor-tests` (the editor leg still
builds), the ones the list names; taking a lane off the list puts it back in the queue, and the lane
(a job) must join `check`'s `needs` too. Lint and the platform lanes run in the queue whatever the list says. The advisory run a push to
`main` dispatches runs what the queue left out instead, as the queue would have (below), so every
merged change still gets their results, in that run's report and the `advisory-red` issue.

### How the MCP shards are balanced

`changes` plans the `mcp` lane's shards (`tools/ci/plan_lanes.py`, `tools/mcp/shard.py plan`): each
file is one chunk (one editor, its tests in order), a `TESTS_INDEPENDENT` file's tests are cut into
chunks of about 10 min, and the chunks go longest first to whichever of a shard's two workers ends
first. The lane ends with its slowest shard, so the times have to be right:

- **Committed timings first**: `tools/mcp/ci-timings.json` holds each CI-suite file's seconds (its
  tests summed) and each test of a `TESTS_INDEPENDENT` file, medians of PR and merge-queue shards. A
  file's tests without a time of their own share what its seconds leave (a test split since the
  refit keeps the file's total), at least the file's default when some are timed. Then the nightly's
  per-test seconds (the `mcp-timings` cache) for the files it lacks, then its default a test (the
  suite's mean). The nightly plans by its own seconds alone: its shards build the games a PR's skip.
  An unreadable file is left out, and the report's fallbacks say so.
- **The refit**: `mcp-timings-refit.yml`, daily and on demand, runs `tools/ci/mcp_timings.py fetch`
  (the `mcp.json` of the newest 20 ci.yml runs of this repository's own branches) and `refit`: one
  sample per run of each file, the median after dropping samples over 3 times or under a third of it,
  nothing from a single sample, and a new value only outside 25 % (at least 15 s) of the committed one.
  When an entry moved, it force-pushes `ci/mcp-timings-refit` and opens or updates its pull request
  (`tools/mcp/ci-timings.json` is on `CI_NOTHING_READS`: its PR runs only `changes` and `Check`).
  The GitHub App `jasper-ci-bot` (secrets `CI_BOT_APP_ID`, `CI_BOT_APP_PRIVATE_KEY`) pushes, opens
  the PR and turns on auto-merge: the workflow token may not open pull requests here, its push starts
  no pull_request run (whose `Check` the ruleset requires) and its auto-merge does not enqueue. The
  App's push runs CI as any PR's, and the merge queue lands the PR once `Check` is green.
- **The shard count**: the fewest shards that no larger count (up to 8 on a PR and in the queue) beats
  by more than the 4 min of setup each added shard costs (checkout, image, editor install: median
  236 s over 46 shards). Once the longest chunk sets the length, more shards save nothing.
- **A long test** is a piece no plan splits: the Title Art library's gate (52 to 103 min as one test)
  runs as `test_gate_<k>_of_8`. The longest chunk now is `test_voxels.py` (38 min).

From the PR and merge-queue runs of 2026-11-19/20 (`tools/ci/mcp_timings.py fetch`, then each plan
replayed on the measured medians): planned without timings (15 of 20 runs: the cache was gone) the 8
shards took 23 to 99 min; by the nightly's seconds (its game builds an hour each, a PR's skip them)
18 to 81 min in 7 shards; by the committed timings, 7 shards of 38 to 42 min.

### What each event runs

| | PR push | merge queue | push to `main` | advisory run | `full=true` dispatch |
|---|---|---|---|---|---|
| `changes`, `Check` | yes | yes | yes | yes | yes |
| `lint` | unless every path is one nothing reads | same | no | no | yes |
| `linux`, `web`, `android`, `windows` | when anything that builds changed | yes | yes, to warm the caches (`windows`: the editor leg's build only) | `linux` when `mcp` or `smoke` runs (their editor install); `windows` for `windows-editor-tests` (the editor leg only) | yes |
| `windows (engine-d3d11)`, `windows (engine-vulkan)` | with `windows`, when one of their inputs changed (below) | the same; the nightly builds both every night | yes (no build; `engine-vulkan` saves the Vulkan SDK's cache) | the same as the queue | yes |
| `macos` | when an Apple-only input changed (below) | yes | yes, to warm its cache | no | yes |
| `ios` | when an Apple-only input changed (below) | the same; the nightly builds it every night | no (no cache) | no | yes |
| `unit`, `htmlui` | the suites the plan selects | all, unfiltered; not while advisory | build only (the caches, the deps dumps) | while advisory all, unfiltered | all |
| `asan` | the unit lane's suites when a path the sanitizers see changed; every test when the lane's settings did; else no (below) | all, unfiltered; not while advisory | build only (each binary's cache) | while advisory all | all |
| `mcp` | the planned CI-suite files | the planned files, over the group's diff against `main`; not while advisory | no | while advisory the planned files, over the diff since the last advisory run | the whole CI suite |
| `smoke` | when a game-building path changed | yes; not while advisory | when a game-building path changed (its cache) | while advisory yes | yes |
| `windows-editor-tests` | with the Windows editor leg | the same; not while advisory | no | while advisory yes | yes |

- **Nothing reads it**: a change only under `docs/specs/`, `docs/superpowers/`, `docs/engine-gaps/`,
  `docs/openengine-analysis/`, `docs/test-cleanup/`, `.claude/`, a root `*-LEDGER.md`, `README.md` or
  `tools/mcp/ci-timings.json` runs only `changes` and `Check`, on a PR and in the queue. The rest of `docs/` counts as building:
  the editor embeds the MCP guide, and tests read other docs.
- **Ledgers** ride in the PR of the work they record, so they cost no run of their own.
- **Blocking lanes only, while GitHub's runners are the bottleneck**: with the repository variable
  `PR_ADVISORY_LANES` set to `off`, a pull request plans as the merge queue does: its advisory test
  lanes and test runs (`ADVISORY_LANES`: unit, ASan, HTML UI, MCP, smoke, the Windows editor tests) are
  planned off (`advisory_off`; the editor leg still builds), and its `Check` waits for lint and the
  platform builds only. They run on `main` after the merge, in the advisory run a push dispatches, as
  for the queue. The default (`on`, or no variable) runs them. Set it while the organization's 60
  concurrent hosted jobs are saturated, delete it when the backlog clears:
  `gh variable set PR_ADVISORY_LANES --body off`, `gh variable delete PR_ADVISORY_LANES`.
- **Stricter than check-changed**: any building change runs every platform lane, Web and Android
  included; a `.github/` change runs everything (ASan as below); a plan error, a missing deps dump or a missing or
  stale coverage map widens that lane to everything, and the report says why.
- **ASan on a PR** (2.5 to 5 runner-hours a run) runs the unit lane's selection only when a path the
  sanitizers see changed (`tools/ci/plan_lanes.py` `SANITIZER_INPUTS`): C/C++ sources and headers
  (`.inc` included), CMake files and presets, `docker/`, `tools/build.sh`, `tools/with-xvfb.sh`, the
  gtest gate, and the vendored libraries (`libs/`, `engine/libs/`, `editor/libs/`). When the lane's
  own settings or definition changed (`tools/sanitizers/`, `ci.yml`'s `asan` job or the workflow's
  `env`, found from the diff's lines), it runs every test of both binaries (`asan_filter_*`), since no
  selection sees them. Anything else, such as CI's other jobs and scripts, docs, MCP tests, shaders,
  assets and a test's data, skips it on the PR. The advisory run on `main` runs it after the merge, and
  shadow mode judges an ASan failure there against the PR's ASan selection.
- **macOS and iOS on a PR** build only when an Apple-only input changed (`tools/ci/plan_lanes.py`
  `APPLE_INPUTS`): the Apple and Mac platform code (`engine/platform/apple/`, `engine/platform/mac/`),
  the Xcode project (`engine/workspaces/xcode/`) and the scripts that sync its source lists
  (`tools/xcode_sync_*`, `tools/test_xcode_sync_sources.py`), the Metal shader stubs it compiles
  (`engine/shaders/msl21*`), and what the lanes run (`tools/ci/macos.sh`, `platform-common.sh`,
  `retry.sh`, the report action). A `ci.yml` change alone does not count, even one to their own jobs.
  The reason is capacity: the organization has 5 macOS runners, and on 2026-11-23 macOS jobs waited a
  median of 2 h (up to 10) while the merge queue stalled behind them. The queue is the gate: it, a push
  to `main`, full runs and the nightly build `macos` for every change that builds, so a macOS break
  still stops the merge, one queue run later than a PR would have shown it.
- **The Windows editor leg in the merge queue** builds the editor and its call CLI (what `windows
  (compile)` and the CLI check use), not the editor-tests binary: its test run is advisory there
  (`lanes.json` `unbuilt`; `tools/ci/windows.sh editor` with `EDITOR_TESTS_BUILD=0`). The tree is
  still configured with its tests, so every object is the one the ccache entry holds. The binary was
  558 of the step's 3,957 build steps, about a quarter of its 110 min (run 45291112468). A pull
  request, a push to `main`, the advisory run and full runs build it. An MSVC-only compile error in an
  editor test therefore shows on `main` after the merge, in the advisory run, not in the queue.
- **macOS in the merge queue** builds only for a macOS-only input. macOS is what caps the queue: 5
  hosted runners and `je-mac`, a `macos` job a group. Its inputs, wider than a pull request's (the queue
  is the last gate), are:
  - `APPLE_INPUTS`, plus Objective-C sources (`*.mm`, `*.m`), Mac and Apple named sources and
    resources (`*/Mac[A-Z]*`, `*Mac.cpp`, `*/Apple*`, `editor/resources/macos/`), the vendored sokol
    (its Metal backend), `tools/ci/macos.sh` and the ccache actions it runs;
  - a C/C++ source whose text, before or after, names `__APPLE__`, `TARGET_OS_`, `SOKOL_METAL` or
    `__OBJC__`;
  - a CMake file whose changed lines or their 10 lines of context name an Apple setting (`APPLE`,
    `IOS`, `CMAKE_OSX`, `Darwin`, `XCODE`, macOS, Metal);
  - `ci.yml`'s `macos` job or `env` (`tools/ci/plan_lanes.py` `MACOS_PATHS`, `macos_inputs`).

  Without one, the lane is planned off (`queue_off`), so a mac-only break (a compile error or a CPU
  test) shows on `main` right after the merge, not in the queue. The push to `main` runs the whole lane
  then (`macos_full`: its CPU tests, the call CLI and the compile smoke, which a push otherwise skips
  because the queue ran them), and the nightly builds it every night. Replayed over the 40 merges up
  to #181 (38 that build), the queue would have built `macos` for 13 (34%) and left it out for 25;
  today's mix was CI-heavy.
- **iOS in the merge queue** follows the same rule (`lanes.json` `queue_off`, which `Check` accepts as
  a planned skip). `ios` is a compile check of the Xcode project with the same Apple Clang the queue's
  `macos` lane builds every change with, and macOS capacity is the bottleneck. `nightly.yml`'s `ios`
  job builds the project for iOS and macOS every night, whatever changed, so an iOS-only break shows
  there within a day; the nightly's report opens or updates its issue. Full runs build it, a push
  does not (it keeps no cache). There are no iOS
  CMake toolchain files: `ios` builds the Xcode project, not CMake.
- **MCP in the merge queue** is planned from the coverage map, not the whole suite: a regression the
  map misses is the nightly's to find. A planned file outside the CI suite (it needs the fixture
  project, or is `CI_EXCLUDED`) is listed under "not run in CI"; `MCP_SUITE=all` locally covers it.
- **The Windows D3D11 and Vulkan legs** (56-97 min each in merge-queue runs 45115998605 to 45216542055,
  and `windows-compile` waits for every leg, so they set the queue's floor) build on a PR, a dispatch,
  in the merge queue and the advisory run only when one of their inputs changed (`tools/ci/plan_lanes.py`
  `windows_legs`, `lanes.json` `windows_parts`, the `windows` matrix): the sokol renderer
  (`engine/renders/`) and its vendored library (`engine/libs/sokol/`), the shaders (`engine/shaderlib/`,
  their stubs in `engine/shaders/`), the shader compilers and translators (`engine/libs/shadercompiler/`:
  glslang, SPIRV-Cross), the shader data, variants and pools (`engine/core/shader/`, `ShaderPool`,
  `ShaderRender`), the Windows platform code (`engine/platform/win/`; `VulkanContext` for Vulkan), the
  engine's presets and CMake modules, `tools/ci/windows.sh`, `platform-common.sh`, the report action, and
  `ci.yml`'s `windows` job or the workflow's `env`. Also a changed C/C++ file the legs compile (the engine,
  `editor/procmat/`) whose text has backend code: a `SOKOL_<backend>` guard, D3D11 or DXGI for D3D11,
  Vulkan, SPIR-V or the `vk` API for Vulkan; and a changed CMake line that names a backend or Windows, or
  sits in an `if()`, `function()` or `macro()` that does (a source list's line is every backend's). The
  reason: the two legs compile the engine and its core tests as `engine-glcore` does, with the same MSVC,
  for one more backend, and run no test (no GPU on the runner), so all they catch is code that compiles
  for that backend alone. Generic engine code rarely breaks one backend alone; backend and shader paths
  still trigger them; and `nightly.yml`'s `windows-engine` builds both every night, whatever changed, so
  a break the rule missed shows within a day in the `nightly` issue. A push, full runs and the rollback
  (`CI_FULL_SELECTION`) keep every leg. `windows (engine-glcore)` runs whenever the lane does.
- **Push to `main`** builds what keeps a cache and nothing else (`tools/ci/plan_lanes.py` `build_only`
  lists what builds without its tests): it saves the caches (ccache per lane), and a PR restores what
  `main` saved, never what the queue did; and it publishes the deps dumps `changes` plans from (below,
  "Plan inputs"). The queue built and tested the same commit, so a push runs no check, nor any step
  the queue ran (`MAIN_STAGE` in `ci.yml`). ASan builds both binaries for their entries, the Windows
  engine legs build without their tests and `windows (compile)` and macOS make their Compile builds,
  for theirs, and `smoke` runs when a game-building path changed, for its
  entry.
- **One push run at a time.** Merge-queue groups land one push each, some minutes apart (2026-11-15
  to 20: 21 pushes, gaps of 62 s, 3 and 4 min among 20). Push runs share one concurrency group without
  cancel-in-progress: one runs, the newest waits, and GitHub drops an older waiting run before it takes
  a runner. A push run's `changes` job first waits `ADMISSION_SECONDS` (300) and steps aside, running
  no lane (`superseded_by` in `lanes.json`), when `main` moved on by then with a path that builds: the
  newer run builds and saves. A newer push that touches only paths nothing reads does not supersede it.
  Each push run plans against the head of the last push run that finished (`gh run list`), so a dropped
  run's change is still built.
- **The advisory run.** A push that builds dispatches `ci.yml` with `advisory: true` on `main`. That
  run, named "Advisory tests of main at <sha>", runs the tests of the `ADVISORY_LANES` lanes the queue
  left out, as the queue would have, plus the jobs they need (above). Advisory runs share their own
  concurrency group: one at a time, the newest waiting. Each is planned against the head of the last
  advisory run that finished, so one run tests every push since that one, and its report starts
  "Advisory tests of the changes since <sha>". With no earlier run, it runs every advisory lane and
  the whole MCP CI suite. `Check` stays green on an advisory failure, so the run keeps one issue,
  labelled `advisory-red`: opened or commented on by each run with a failing, cancelled or missing job,
  closed by the first green one. A run takes about 4.5 h (ASan). Over the pushes of 2026-11-15 to 20,
  a model with 270 min runs gives each merged change its results 5.7 h after it lands on average (at
  most 8.8 h), in 15 runs instead of 21, never two at once. Running them in every push run took 4.5 h
  but up to three runs at a time (five or six at two to four times the pushes); an hourly schedule
  averaged 5.9 h (7.5 to 7.9 h at two to four times the pushes). The unit, ASan and HTML UI jobs build
  again in the advisory run, from the entries the push runs saved (an advisory run saves none).
- **ccache in CI**: a build runs with no size limit (a cleanup in the middle of a build evicts the
  restored entries it has not reached yet, and each miss then evicts more: a PR that changed one test
  file hit 5 of 5,279 compiles). `ccache-save` then evicts every entry the run neither used nor wrote
  and prints the rest, the lane's working set, on every run; on `main` it cuts a working set over the
  lane's cap (`max-size` in `ci.yml`) to it, oldest-used first, and keeps one under it whole (ccache's
  cleanup evicts from each of its 16 subdirectories past a 16th of the cap, so under the cap it still
  cost the next linux build 238 of 5,284 compiles), recompresses it from the zstd level 1 a build
  writes to level 9 (14 % smaller: a linux sample of 296 MB took 23 s on 2 CPUs; level 12 gave 15 %,
  19 22 % in 277 s), saves it and deletes the lane's older entry (`prune`;
  the `caches` job, after every lane, deletes them again). Each lane's old and new entries would
  otherwise both live until the push's slowest lane ended, and past the 10 GB GitHub evicts the least
  recently used entries of any lane: during push run 44629653601 the repository held 11.7 GB, two
  entries of linux, unit-core and Android, and the plan inputs, the coverage map and the Vulkan SDK
  were gone. A cap below the working set loses the start of the build, not all of it. The caches
  share the repository's 20 GB (`max_cache_size_gb`, raised from GitHub's default 10 on 2026-11-24); working sets from each
  lane's `ccache-save` (no precompiled headers, line tables only: `JASPERENGINE_DEBUG_INFO=lines`,
  "Build speed"), in decimal GB (ccache's caps are decimal; `du -h` prints binary), from PR 131's
  runs 44775260806 and 44806837004:

  | Cache | Working set (GB) | With full debug information | Cap | Entry |
  |---|---|---|---|---|
  | linux | 0.87 | 1.5 | 0.95G | yes |
  | unit-core | 0.37 | 1.34 | 0.4G | yes |
  | unit-editor | 0.80 | 4.0 | 0.85G | yes |
  | asan-core | 0.74 | 2.1 | 0.8G | yes |
  | asan-editor | 1.58-1.62 (the whole build: 7,721 entries of 3,863 compiles; runs 44934269450, 44959473600, 44970821863 and a local build) | 4.6 | 2.2G | yes |
  | htmlui | 0.78 (2,176 compiles, Release) | 1.66 | 0.85G | yes |
  | macOS | 0.81 with the Compile build, which a push makes for it (PR run 45075106333) | 1.30-1.37 | 0.9G | yes |
  | Windows editor | 0.92 (MSVC: `/Z7` as before) | 0.91 | 0.95G | yes |
  | Windows OpenGL | 1.50 (1.4 GiB; `/Z7`: its core tests' reports name frames), 1.29 recompressed; PR 176's run 45274919394 | | 1.6G | yes |
  | Windows D3D11, Vulkan | 0.35 each (337 MiB; Release, no debug information), 0.31 recompressed; same run | | 0.4G each | yes |
  | Windows Compile | 0.45 (425 MiB), 0.38 recompressed; same run | | 0.5G | yes |
  | iOS and macOS Xcode (`xcode-cas`, Xcode's compilation cache, no debug symbols) | 1.14-1.23 raw, 0.35-0.38 compressed (both SDKs, five builds of two commits 36 apart, on the Mac); saved by the nightly's `ios` job on `main` (a push would take a macOS runner for a lane the queue mostly leaves out), and it only grows, so a nightly that restores over 2 GB starts it afresh (`COMPILATION_CACHE_LIMIT_SIZE=1600M` held it at 1.78 GB with 92 % hits on a commit seen before, against 100 % without) | iOS alone 0.89 compressed | 2 GB raw (`fresh-over`) | yes |
  | Android | 0.27 | 0.63 | 0.3G | yes |
  | web | 0.24 | 0.48 | 0.3G | yes |
  | smoke | 1.93 (the player runtime and two Compile builds, which share nothing: each its own directory) | 3.5 | 0.5G | yes, the last-used part: the second Compile build |
  | Gradle, Vulkan SDK, MCP timings, uv | 0.73, 0.21, under 0.01 each | | | yes |

  The caps add up to 11.9 GB in ccache's sizes; a saved entry is recompressed to about 0.86 of its
  working set (the Windows legs: 0.85-0.89), and GitHub counts the saved archives, about 0.96 of that
  again (asan-editor: 1,526 MB for its 1.59 GB). At these working sets the ccache entries come to
  about 8.6 GB and the rest (Gradle, the Vulkan SDK, `xcode-cas`, MCP timings, uv) to 1.3 GB: main's
  entries hold about 10 GB, and caps full everywhere about 11 GB, which leaves the 20 GB room for a
  push's old and new entries side by side and for the other branches' caches (the Vulkan SDK of a PR,
  the merge queue's Gradle) without GitHub evicting one of main's. A cap a few percent above its working set still cuts it: ccache 4.5 (the image's) empties
  each of its 16 subdirectories that is over a sixteenth of the cap down to 80 % of that
  (`limit_multiple`), whatever the total, and the largest subdirectory runs about a quarter above the
  mean. asan-editor's whole build (1.58 GB, the largest subdirectory 123 MB) lost 415 of its 7,721
  entries to a 1.7G cut (1.8G: 214, 1.9G: 131, 2.0G: none; `ccache --cleanup` on a copy), and the
  push run restored from it hit 93.8 % of its compiles; restored from a 1.6G cut of its 1.61 GB, it
  hit 88.9 % against 96.4 % uncut. Hence its 2.2G: 16 times that subdirectory, and a tenth to grow.
  The other lanes' caps, about 5 % above their working sets, lose a few percent of their entries the
  same way. Smoke,
  last in line (the slowest cold lanes that run on every PR came first: unit-editor, then ASan),
  takes what is left and keeps its last-used part. A cache with no entry builds cold, its job still
  reusing what an earlier step of the same job compiled. A cancelled job saves nothing (the Windows
  and macOS lanes): its working set lacks what it had not reached. A lane that
  gives its entry up has its prefix in the `caches` job's second list, so its last entry goes at
  once instead of holding the budget for 7 days.
- **Full run on any branch**: `gh workflow run ci.yml --ref <branch> -f full=true`.

### Plan inputs

`changes` plans a PR from two inputs made on `main`: the Ninja deps dumps of the three test trees
(`tools/ci/dump_deps.sh`, by the unit and htmlui lanes on every push) and the MCP coverage map (the
nightly's coverage shards). They live on the data branch `ci-plan-inputs`, not in the cache: GitHub
evicted the cache entries of both for days at a time (of the 135 PR and merge-queue runs from
2026-11-14 to 2026-11-20, 11 had a map; of the 67 PR runs after the first deps dump was saved, 11 had
none), and each missing input widened its lane to everything. `tools/ci/plan_inputs.py`:

- `publish` replaces the branch with one parentless commit holding `manifest.json` (each input's
  `main` commit and run), `coverage-map.json.gz`, `deps/<tree>.deps.gz` and `shadow.jsonl` (below),
  over what the branch held; a push another job won is retried on top of it. The deps dumps are
  compacted first (every object with its dependencies inside the repository, a vendored library's
  with its source: 152 MB of dumps become 34 MB, 1.6 MB compressed), and select the same suites
  (`tools/ci/test_plan_inputs.py`; 150 sampled sources of the real dumps, 0 differences).
- Writers: the `plan inputs` job of a push to `main` (the deps dumps) and the nightly's `Coverage
  map` job on `main`, the only jobs with `contents: write`, running `main`'s code. The branch is
  outside the `main` ruleset; a PR, a fork or another branch's run never writes it.
- `fetch` (the `changes` job, read-only) writes what the branch has into `build/ci-inputs`; no
  branch or no input fetches nothing, and the plan widens as before. Locally:
  `python3 tools/ci/plan_inputs.py fetch --out /tmp/inputs`, then `tools/check-changed --deps-from
  /tmp/inputs/deps` with `JASPER_MCP_COVERAGE_MAP=/tmp/inputs/coverage-map.json`.
- Freshness: the map is remade every night and a map older than 14 days or 400 commits still widens
  (`MAP_MAX_AGE_DAYS`, `MAP_MAX_COMMITS`); the deps dumps on every push to `main`.

### Why a test runs

The plan records a reason for every suite and MCP file it selects (`check_changed.REASON_KINDS`), in
`check-changed.json` (`unit_reasons`, `mcp_selected[].kind`) and `lanes.json` (`selection`), and the
`changes` job prints it in its summary (`selection.md`: a row per test lane, the first 40 reasons,
what widened the plan and the plan inputs' commits and when they were published):

| Kind | A suite or file runs because |
|---|---|
| `owner` | its own source changed (a test unit, an MCP test file) |
| `dependency` | it includes what changed (Ninja's deps), or runs, calls or loads it (the coverage map, a tool's name, a path) |
| `policy` | a rule watches the changed path: a shader's `*Gpu` suites, the compile and game-build files, the save guard, the agent-docs and guide checks |
| `smoke` | it is one of the 10 quickest files that run code nearly every editor runs |
| `full` | the event runs the whole suite (merge queue and advisory run units, a full run, the nightly) |
| `fallback` | a widening ran the whole suite, and says which (no map, a stale map, a new source, CMake, a test's data, `.github/`) |

**Shadow mode** measures what the selection would miss before anyone trusts it further. A run that
runs more than a PR would (every unit test: the merge queue's, or the advisory run's while the unit
lane is advisory; a `full=true` run; the nightly: the whole MCP suite) carries in `lanes.json` a `shadow`: what a PR's selection of its diff would run
(`plan_lanes.py shadow_selection`; the nightly's diff is the one since the coverage map's commit,
`--shadow-base`, planned with the inputs a PR would have had). `tools/ci/shadow.py check` judges each
failed test of a scope the run ran whole (unit core, editor, HTML UI, MCP) against it, an ASan job's
against the PR's ASan selection (`shadow.asan`):

- **caught**: the PR selection runs it; **miss**: it leaves it out; **pre-existing or flaky**: left
  out, but the history saw it fail within 7 days; **no change to blame**: the diff is empty.
- `ci.yml`'s `shadow` job (merge queue, push, dispatch, the advisory run included; read-only) prints the record in its summary
  and uploads it as the artifact `shadow`; the nightly's `Shadow selection` job judges its own MCP
  run, gathers the `shadow` artifacts of CI's runs since its last record, appends them to
  `shadow.jsonl` on `ci-plan-inputs` (the tracked report, the newest 500 records) and prints
  `shadow.py report` of it. `python3 tools/ci/plan_inputs.py fetch --out /tmp/i && python3
  tools/ci/shadow.py report --history /tmp/i/shadow.jsonl` reads it anywhere.
- A miss is a selection gap until shown otherwise: find why the selection left the test out (a
  dependency no input records), and fix the rule with a test in `tools/test_check_changed.py`.
- Replayed over the 50 merge-queue runs with plans from 2026-11-16 to 2026-11-20 (their
  `check-changed.json` against their unit, HTML UI and MCP reports): 422 failures caught, 91
  pre-existing or flaky, 6 misses: five HTML UI tests at their first failure in run 43871305894 (a
  change with no HTML UI file; they failed in every later run that ran them, so `main` had broken
  them), and `PhysicsContactsTest.ContactProcessingCostIsSmall` (a cost budget, failed again two days
  later on an unrelated change).

**Rollback**: the repository variable `CI_FULL_SELECTION` set to `true` turns the selection off: a PR
or merge-queue run whose change builds runs every lane whole (unit and HTML UI unfiltered, the whole
MCP CI suite), an advisory run its lanes so, and its plan says so in `fallbacks`; prose nothing reads
still runs nothing.
`gh variable set CI_FULL_SELECTION --body true` sets it, `gh variable delete CI_FULL_SELECTION` lifts
it; the next run's `changes` reads it.

### The nightly and releases

- **Nightly** (`nightly.yml`, 03:17 UTC, or dispatched): the slow configurations (HTML UI on Vulkan
  and under ASan, TSan, texture formats on lavapipe, the unity builds of the editor, the engine and
  both unit-test trees, every fixture's smoke on OpenGL, GLES3 and Vulkan through
  `tools/ci/nightly-smoke.sh`, the pthreads web gate, prebuilt player parity, the Windows engine and its core tests built on
  D3D11 and Vulkan, `windows-engine`) and the whole MCP CI suite, which also refreshes the coverage map (below, "Plan
  inputs") and the nightly's own shard timings (the `mcp-timings` cache). A failure opens or updates the issue
  labelled `nightly`. When `main` moved, it publishes the prerelease `nightly-YYYYMMDD` (the newest 14
  are kept); the slow lanes do not hold it back.
- **Stable** (`release.yml`, dispatched with a `vX.Y.Z` version, an optional commit that defaults to
  the newest nightly's, and `dry_run`): tags the commit, checks the tag
  (`tools/ci/check-release-tag.sh`) and builds it.
- Both build through `build-release.yml`: every player variant of `engine/player/variants.json`, then
  the editors embedding their hashes, launch checks, `SHA256SUMS`, and a draft release that is
  published once every asset is attached. Everything is unsigned.

### The report

- **On a PR**: one comment, marked `<!-- jasper-ci-report -->` and updated by every run. It names the
  commit, every failed lane with its failing build lines or tests, the repro command, the evidence,
  and what did not run and why; when green it is one line. `report` in `ci.yml` writes it;
  `ci-report.yml` writes it for a pull request from a fork, whose run cannot.
- **Merge queue**: a group that fails removes the PR and posts the same report on it, prefixed
  "Removed from the merge queue". The commit it names is the queue's merge commit.
- **Advisory run**: its report heads the job summary; a failing, cancelled or missing job opens or
  comments on the issue labelled `advisory-red`, and the first green run closes it (above).
- **`main`**: a failed push opens or updates the issue labelled `ci-red`, and comments on the PR that
  made the commit.
- **Each run**: the same text in the job summaries; per lane the artifacts `ci-report-<lane>` (JSON:
  failures, test counts, commands, timings, disk) and `test-results-<lane>`, kept 14 days.

```sh
gh pr checks <N> --watch                       # wait for Check
gh pr view <N> --comments                      # the report
gh run download <run> -p 'ci-report-*'         # its JSON, per lane
```

### Test time budget

`tools/ci/time-budget.json` holds what CI time a test may cost: per suite (`core`, `editor`, `htmlui`,
`mcp`, by lane) a cap for one test, for MCP also for one file, and the suite's total on a complete run
(no filter: the merge queue's unit and HTML UI lanes, the editor's two parts added up); the tests and
files above a cap are listed with their measured seconds and a reason. `report.py collect` measures
each lane's tests against it and the PR comment lists what is over, marked advisory: `Check` does not
count it. A listed test may grow by `tolerance` (2: one runner measured the same tests 1.4 to 1.7 times another), a total by `total_tolerance` (1.5).
What is over gets cheaper (above, "Writing load-robust tests"; one owner per contract, a smaller check
where it covers the same contract) or its budget raised in the PR, with the reason a reviewer reads:

```sh
python3 tools/ci/time_budget.py check --lane mcp-3 build/test-results/mcp.json      # what the report would list
python3 tools/ci/time_budget.py allow mcp test_x.py::test_y 300 --reason "why it must cost this"
python3 tools/ci/time_budget.py ratchet --lane unit-core build/test-results/core.xml # lower what got cheaper
```

Its tests: `python3 -I tools/ci/test_time_budget.py`.

### Reproducing a lane

- Run the printed command. It is the lane's command narrowed to the failure, for example
  `UNIT_SUITE=core UNIT_TEST_ARGS='--gtest_filter=MeshBvh.Refit' WERROR=1 tools/build.sh unit-tests`
  or `MCP_TEST_ARGS='-k test_undo_paint' tools/build.sh mcp-tests tools/mcp/tests/test_tileset.py`.
- Linux, Web and Android lanes run `ghcr.io/<owner>/jasperengine-<target>-build:<hash>`, where
  `<hash>` is `tools/build.sh image-tag <target>`, the hash a local build tags its own image with;
  `JASPER_DOCKER_IMAGE=<that name>` runs CI's copy.
- An MCP shard: `tools/build.sh linux`, download the run's `plan` artifact into `build/ci-plan`, then
  `tools/ci/mcp_shard.sh <k> build/ci-plan`.
- Windows: `tools/ci/windows.sh <step>` in Git Bash inside an x64 Native Tools prompt. macOS:
  `tools/ci/macos.sh <step>` with Xcode.

### Job setup

What a job does before its build or test step, paid by every job of a run (about 30 Linux jobs on a
PR or in the queue, about 100 in the nightly).

- **Free disk** (`.github/actions/free-disk`): an `ubuntu-latest` runner starts with about 87 GB
  free of 145 (2026-11-20). The action removes the preinstalled Android SDK, .NET, GHC, CodeQL and
  Docker images (about 26 GB, 1-2.5 min) only when `/` has less free than the job's `need`: its
  largest use of `/` seen in its reports' `disk` line (the build tree, the image and the caches), plus
  a quarter and 10 GB. Measured use in GB (2026-11-20, PR, queue and nightly runs) and the `need`
  set:

  | Job | Use | `need` |
  |---|---|---|
  | `asan` (editor; core 30) | 55 | 80 |
  | `android` (every ABI, queue) | 44 | 65 |
  | `unit` (editor; core 19) | 40 | 60 |
  | `smoke` | 33 | 55 |
  | `web` | 17 | 35 |
  | `linux`, `htmlui` | 14, 13 | 30 |
  | `mcp` | 5 | 20 |
  | nightly `slow` (TSan; the others 5-30) | 56 | 80 |
  | nightly MCP and coverage shards | 23 | 40 |
  | nightly `parity` | 87 | none: always frees |
  | release players Linux, Android, web | 44, 49, 12 | 65, 75, 25 |

  A job without `need` always frees. A job whose `need` is not met after the removal gets a warning
  in its log; raise its `need` when its reported use grows.
- **Image** (`.github/actions/image`): pulls `ghcr.io/<owner>/jasperengine-<target>-build:<tag>`
  anonymously (the packages are public) and logs `image: pulled <ref> in <n> s`. A tag the registry
  does not have (a PR that changes `docker/`, until its merge runs `images.yml`) is built in the job
  from the registry's layer cache and logs `is not in the registry; building it`, then
  `image: built <ref> in <n> s`; a missing tag is asked for once, any other pull failure twice.
  `images.yml` pushes zstd layers, which unpack about 4 times as fast as gzip (Docker 23 or later
  pulls them; `JASPER_DOCKER_IMAGE=<ref>` needs one locally); tags pushed before that stay gzip until
  the weekly rebuild.
- **Editor install** (artifact `editor-install`, from `linux` to `mcp` and `smoke`; the nightly's
  `nightly-editor-install`): `tools/ci/editor_install.sh pack` leaves out the folders the install copies
  unchanged from the checkout (the Title Art HDRIs, fonts and thumbnails, the sound library: half the
  archive, already compressed), and `unpack` copies them back from the job's checkout of the same
  commit: 199 MB to 100 MB. A folder the build changed travels in the archive.
- **Checkout**: depth 1, except `changes` (every commit, `filter: blob:none`), `mcp` (depth 2: the
  title golden ledger compares the merge commit with its base) and the nightly's and the release's
  build jobs (every commit and tag for `git describe`, `filter: blob:none`: the history without the
  old file contents).

### Overflow runners

Three machines of the project's owner take CI jobs when GitHub's runners of their OS are all taken:
`je-mac` (Apple Silicon), `je-linux` (x86-64, Docker) and `je-win` (x86-64, 8 cores and 7 GB). Each is
one repository runner (one job at a time), labelled `self-hosted`, its OS and arch, and its `je-*`
label, running as the machine's normal user under its runner folder (`~/je-runner`, `C:\je-runner`) as
a service that starts with the machine (macOS: with the user's login) and reconnects after sleep.
`tools/ci/self-hosted/` installs it (`install-runner.sh`, `install-runner.ps1`, `windows-tools.ps1`).

**Hosted first.** `changes` routes a job (`tools/ci/plan_lanes.py` `route`; `ci.yml` reads
`runs_on.<job>`, the hosted label when it is missing) to an overflow runner only when all of these hold:

- the run may use one: a push, the merge queue, a dispatch, or a pull request whose head repository is
  this one, never a fork's;
- it is not the rerun of a run the watchdog moved off a runner that went away (the state's
  `hosted_only`, below), nor of a forced dispatch: those stay on GitHub's runners, so they never wait
  for the same machine again. A rerun for an idle runner (below) routes as a first attempt does;
- the runner state (`state.json` of the data branch `ci-runner-state`, fetched as the plan starts; not a
  repository variable, which a run reads as it is created, up to an hour before `changes` gets a
  runner) is under 5 min old (`STATE_MAX_AGE`), and says the hosted
  runners of the job's OS are saturated: a job of that OS waits while its limit is used up (macOS: 5
  running; any OS: 60 hosted jobs of the organization), or has waited 5 min. The merge queue, the
  critical path, takes the Mac as soon as any hosted macOS job waits;
- the runner is online, idle (no job running or queued for it) and fit (its disk has `min_free_gb`),
  and `SELF_HOSTED_DISABLED` does not name it;
- the job is on the runner's list (`OVERFLOW`, best first), within what the runner takes of one plan
  (`TAKES`, one by default): `je-mac` takes `macos` and `ios` (it builds `ios` in 12 min, run
  45104498209), one after the other; `je-linux` `linux` (the MCP and smoke lanes wait for it), else the
  longest build; `je-win` the D3D11 or OpenGL engine leg. The Windows editor leg's MSVC build does not
  fit in 7 GB in its time. The box reset itself 19 min into the D3D11 leg once (run 45104498209;
  Kernel-Power 41, its sixth such reset in two days, five of them before it ran CI); the watchdog
  reroutes a job it loses that way.

The `changes` job's summary and `lanes.json` (`routing`) say where each job went and why.

**The planner overflows too.** `changes` cannot route itself, and on 2026-11-23 it waited up to an
hour for a hosted Linux runner (79 Linux jobs queued, the oldest for 4 h), so no plan ran at all. The
dispatcher sets the repository variable `SELF_HOSTED_PLANNER` to `je-linux` while hosted Linux is
saturated and `je-linux` is online, fit, idle (no job running or queued for it) and not disabled, and
deletes it otherwise: runs created while it plans go hosted, so planners never queue behind each other
or a build. A run reads
`vars` as it is created, which is when its `changes` job is queued, so the value is current where it
is used. `changes` runs there on a first attempt of a trusted event other than a push (a push waits
`ADMISSION_SECONDS` first). While it plans, `je-linux` takes no lane (the state marks it unfit,
"reserved for `changes`"): every run's `changes` would otherwise queue behind a build. A non-queue run's `changes`
queued 5 min for a busy runner anyway (`PLANNER_WAIT`) is rerouted as for a lost runner: cancelled and
rerun on GitHub's runners. A merge-queue run's is not (a cancel fails its group). If the runner goes away, the watchdog reroutes the run as below, and the rerun
plans on GitHub's runners.

**The state and the watchdog** (`tools/ci/runner_dispatch.py`): a cron job runs it every minute on an
always-on host whose `gh` is a repository admin, from a sparse clone of `main` refreshed hourly. It
reads the runners (GitHub's status, and an ssh probe of each machine's listener and free disk, since
GitHub reports a listener that takes jobs as offline at times) and the jobs of the queued and running
runs, and force-pushes the state to `ci-runner-state` as one parentless commit. Then, for runs that have a job on a `je-*` label only (it never
touches a run it did not route): a job queued 10 min for a runner that is offline or gone, or running
on one offline for 3 min, or failed because its runner went away ("lost communication", "shutdown
signal", read as soon as the job ends), gets its run cancelled and, once stopped, rerun whole (`gh run rerun`): the rerun plans on
GitHub's runners only. A pull request's run that a newer push superseded is not rerun.

**A merge-queue run is cancelled only when its job cannot finish.** A cancel fails its `Check`: the
queue removes the pull request, and every group behind it restarts (run 45310250691, #153's group:
about 2 h lost for a job that would have waited). So the dispatcher cancels a queue run only:

- when one of its jobs already failed because its runner went away (the group is red anyway);
- or when a job has been queued for a runner gone 30 min (`QUEUE_OFFLINE_LIMIT`), not 10.

It does not cancel one for a runner that went offline mid-job, since GitHub fails that job itself
and the first case follows. It never cancels one for an idle runner, a busy planner or anything that
only makes it wait. After such a cancel the pull request goes back to the front of the queue
(`enqueuePullRequest` with `jump: true`, unless it is queued already): a rerun of a removed group
builds nothing.

**Re-routing a waiting job.** A plan routes once, so a job queued on a hosted label stays there even
when an overflow runner becomes idle. When a trusted run's job has waited 15 min
(`IDLE_REROUTE_WAIT`) for a hosted runner while an overflow runner that would take it (`OVERFLOW`) is
online, idle, fit and not disabled, the dispatcher cancels the run and reruns it, so its plan routes
it there, one run per idle runner a minute. Never a merge-queue run's (above). The runs moved off a runner that went away are listed in the state's `hosted_only` for a day,
and their reruns plan hosted only. Each run gets at most one reroute of each kind, and none for an idle
runner after one for a lost runner: never a loop. All of it shares the limit of 6 actions an hour, each
logged to the host's `je-runner-dispatch.log`. With no state for 5 min (the host, `gh` or the cron is down) every
job goes hosted, so an offline machine never holds a run.

**Taking a machine out**: `gh variable set SELF_HOSTED_DISABLED --body je-win` (labels, or `all`);
the next plan leaves it alone, and `gh variable delete SELF_HOSTED_DISABLED` brings it back. A job
already on it finishes; stopping the runner service instead lets the watchdog reroute what waits.
**Testing one**: `gh workflow run ci.yml --ref <branch> -f self_hosted='macos=je-mac windows-engine-d3d11=je-win'`
runs only those jobs' lanes, those jobs on those runners whatever the state says (its rerun: hosted).

**Safe on a person's machine.**

- Each runner's job-started hook (`tools/ci/self-hosted/job-started.sh`, `.ps1`) fails any job that is
  not this repository's push, merge queue, dispatch or same-repository pull request before its first
  step: a pull request runs its own `ci.yml`, so the routing alone cannot keep a fork's code off the
  machine. GitHub also asks a maintainer to approve the workflows of every outside contributor's pull
  request (the repository's fork approval policy).
- The jobs get their own `HOME` and git global config inside the runner folder, so nothing a job
  writes there (Xcode's DerivedData, `brew`'s and `uv`'s caches, `git config --global`) reaches the
  user's own. `free-disk` does nothing there; `brew` and `choco` install only what is missing.
- The job-completed hook empties the job's workspace (only a folder under the runner's `_work`), so
  build trees never pile up. Linux lanes restore ccache from the Actions cache as on GitHub's runners
  (their image mounts the workspace's); on macOS and Windows a runner keeps its own ccache per lane in
  its tool cache, capped at 3 GB (`ccache-restore`; `ccache-save` saves none of it): the hosted entry
  hit 0 of 3,834 compiles on the Mac (other paths, though the same Xcode 26.6) and took 7.5 min to
  download (run 45104498209), where the Mac built the editor cold in 11 min. On Linux it removes the Docker
  images CI pulled (`.github/actions/image` notes them in the runner's tool cache) that no job used
  for 3 days, or every one but the newest of each when under 100 GB is free, and cuts the CI image
  builder's cache to 10 GB; on macOS it empties the runner's home's caches past 20 GB.
- The Mac stays awake (`caffeinate`) while a job runs; a closed lid still sleeps it, and the watchdog
  reroutes.
- Jobs build with `JOBS` (and on Linux `CPUS`) from the runner's `.env`, under the machine's cores, so
  the user's own work stays usable.

### Measured lane times

Warm-cache times are not measured yet: the first runs after CI lands on `main` fill this table, and
set each job's `timeout-minutes` to about 1.5 times its p95.

| Lane | p50 | p95 | Runs |
|---|---|---|---|

Cold builds on a GitHub-hosted `ubuntu-latest` runner (4 vCPU), ccache off, 2026-11-13 (one run each):

| Build | Minutes |
|---|---|
| `UNIT_SUITE=core RUN=0 tools/build.sh unit-tests` | 43 |
| `UNIT_SUITE=editor RUN=0 tools/build.sh unit-tests` | 111 |
| `SKIP_ENGINE=1 tools/build.sh linux` | 73 |
| the same three with `UNITY=1` | not measured on a runner; on a 16-core host, "Unity build (R25)" below |

From the first PR runs (cold): the core tests ran in 65 min on llvmpipe, the editor tests' share is
in the editor leg; under ASan the `*GpuSlow` suites took 135 of 167 test minutes (hence their
nightly job); an MCP shard of about 330 tests took 24 to 86 min without the game builds, and four
game-building tests took 18 to 91 min each, cold.

`unit (editor)` in one job, ccache restored but missing (2026-11-15/16): run 43737549764 123 min (setup
4, build 110, all 5,268 tests 7), run 43773511699 141 min (setup 4, build 137, 39 tests 5 s). The tests
spread evenly (the longest 31 s), so only splitting the build halves the lane: two parts, about
65 min each cold (setup, half the build, the merge and links, half the tests), and about the one job's
time warm.

The ASan lane (`-*GpuSlow*`): CI's core runs stopped at 2,760 tests (`VoxelPhysics.BudgetPerTick`), so
the rest is estimated from the non-ASan times (GPU tests 1.9 times slower under ASan, CPU tests 7.0,
from the tests both measured): the core tests about 132 min, of which five GPU tests 37 min, now
`*GpuSlow` (a full local ASan run, a loaded host: 74 of 222 min); `VoxelSoakSlow.IncrementalEqualsRebuild`
(about 10 min, CPU) stays. The editor tests under ASan: 62 min for the 4,308 a local run reached.

The editor's ASan build with line tables only (`JASPERENGINE_DEBUG_INFO=lines`), cold: 120 and 133 min
for its 3,863 compiles (runs 44934269450, 44959473600), the editor's own sources at 17-19 a minute;
warm, 15 min (run 44970821863). With full debug information a cold run reached 1,555 of them in its
300 min (run 44923917704), the editor's at about 2 a minute. Memory is not what limits the line-tables
build: its heaviest units peak at 3.2-3.7 GB (`WorldTools.cpp`, `InspectorWindow.cpp`, `Project.cpp`;
4.3-5.6 GB with full debug information, the objects 2.3-2.6 times larger) and its compiles average
about 0.7 GB, so Ninja's default 6 jobs fit the runner's 16 GB; the lane sets no `JOBS`.
In one job (merge-queue runs 44416194181, 44487933891, 44582664923, 44582666848) the cold core build took
79-116 min, the core tests 90 and the editor's build 168-175, so the 270-min step ended before the
editor tests: hence a job per binary, cold about 3-3.5 h for core and 3.7-4.6 h for the editor (its
tests estimated).

### Pins

Third-party notices are credits, not a gate: `tools/ci/check-notices.py` stages whatever licence
files and `NOTICE.md` rows exist into the editor install and exported games; CI does not fail on a
library without them.

The Chrome for Testing headless shell in `docker/web.Dockerfile`, and `uv` and Kitware's CMake in
`docker/linux.Dockerfile`, are pinned by version and sha256 and bumped by hand, as are the MCP
harness's packages (`tools/mcp/requirements.txt`). Every workflow action is pinned by its full commit
SHA, with its version in a comment.

### Downloads retry, builds and tests do not

A one-off network failure while fetching something does not fail a job: the images' apt
(`docker/apt-retries.conf`), `wget` and `curl` retry with their own flags, `sdkmanager` in a loop;
Gradle retries through `systemProp.org.gradle.internal.*` in
`engine/workspaces/androidstudio/gradle.properties` (Gradle 9.6 ignores the keys without
`systemProp.`), and the NDK is installed before Gradle with a retry; a workflow's download commands
(`brew`, `choco`, `curl`, `gh release download`) run through `tools/ci/retry.sh`, and the Vulkan SDK
action gets a second step when the first fails. Nothing else is retried: a flaky test gets `DISABLED_`
and an issue.

## Known gaps

What the suites do not cover today, and what closing each gap would take. Each needs money, a
device or a decision; recorded by BF23 (`docs/specs/BF23-authoring-audit-fixes.md`, section 4).

| Gap | Evidence | What it would take |
|---|---|---|
| No native compile check for generated HLSL and MSL | The `windows` and `macos` CI lanes build and run CPU tests only, and `tools/check-changed` has no `fxc`, `dxc` or `metal` step | A step in those lanes that compiles them |
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
`Project` frees its scenes and their renders in its destructor only while the engine runs (the
editor's is destroyed after `Engine::systemShutdown`): a test file's static Project registers with
`editor::test::releaseAtExit` (`editor/tests/support/EditorTestSupport.h`), or every scene and its
render leaks at exit, and `editor::test::closeProject` also drops a loaded project's scene histories.

### Unity build (R25)

`UNITY=1 tools/build.sh linux` and `UNITY=1 tools/build.sh unit-tests` (both suites) build. On a
16-core host shared with other builds, `JOBS=8 CPUS=8 WERROR=1`, ccache off, precompiled headers
on, 2026-11-17, one run each (seconds):

| Tree | Cold | Cold, unity | Scene.cpp edited | unity | Project.cpp edited | unity |
|---|---|---|---|---|---|---|
| `SKIP_ENGINE=1 tools/build.sh linux` (Release) | 1,517 | 960 (-37 %) | 16 | 18 | 31 | 68 |
| `UNIT_SUITE=core RUN=0 tools/build.sh unit-tests` | 954 | 738 (-23 %) | 69 | 82 | | |
| `UNIT_SUITE=editor RUN=0 tools/build.sh unit-tests` | 2,910 | 1,910 (-34 %) | 52 | 50 | 78 | 138 |

An edit rebuilds the 16 sources of its unit, and with ccache a unit misses whenever any of them
changes, or when a source added to or removed from the target moves the batch boundaries after it.
No PR lane builds unity; the nightly's unity job keeps `tools/build.sh linux` unity-safe.

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
