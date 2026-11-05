# CPU hot-path audit

The comparison baseline is `08dea5e0e`. Six cuts remove repeated CPU work.
Measurements below are isolated production-code kernels, not editor frame-rate
improvements. Upstream changes merged during verification are identified separately.

## Critical paths and selection

Code inspection established these call paths (inferred, rather than sampled profiles):

- `EditorFrame::run` calls `App::engineRender` when a view needs drawing, and
  `App::show` for editor interaction. `Engine::runFrame` drives fixed updates,
  variable updates and rendering. Scene updates invoke the ECS scheduler, with
  each built-in subsystem registered as a separate wave.
- `NavigationSystem` replaces each moving grid agent's reservations every fixed
  tick. The reservation book previously scanned and shifted other agents' records
  on every replacement.
- Quaternion rotation matrices feed transforms, bounds, particles, cameras and
  instance preparation. The editor normally builds their math translation units
  separately, without LTO.
- Find References and `ref:` searches call `AssetIndex` reference collection;
  collection holds the index mutex while matching and ordering referrers.
- Console details and selection maintenance repeatedly resolve a selected ID in
  the deque of up to 10,000 retained messages.

The five initial priorities were:

| Priority | Opportunity | Expected impact and risk |
| --- | --- | --- |
| 1 | [Grid reservations](../../engine/core/navigation/GridPathfinder.cpp) | Remove quadratic bookkeeping across moving agents and reuse their path storage; private ownership/expiry semantics need differential checks. |
| 2 | [Singleton scheduler waves](../../engine/core/ecs/SystemScheduler.cpp) | Remove two transient allocations per wave; retain command playback, tracing and exception order. |
| 3 | [Asset reference collection](../../editor/asset/AssetIndex.cpp) | Avoid copying paths during sorting and rebuilding a directory prefix per entry; no persistent cache or invalidation rules. |
| 4 | [Quaternion rotation matrices](../../engine/core/math/Quaternion.cpp) | Replace 16 out-of-line matrix-index calls with the existing constructor; preserve column order and floating-point expressions. |
| 5 | [Console selection](../../editor/window/ConsoleWindow.cpp) | Use ordered IDs for logarithmic lookup; preserve the constant-time newest-message case and eviction behavior. |

An exact-word fuzzy-search shortcut was rejected after mixed results on a
20,000-label corpus. Expanding quaternion/vector cross products was also rejected:
native/FMA differential checks detected changed floating-point rounding, despite
passing generic x86 checks.

A further verified opportunity was [gradient text](../../engine/core/util/TextEffects.cpp): the shared text-effect evaluator
copied and stable-sorted the same already ordered stops for every glyph. Ordered
inputs now use their existing storage; unordered/NaN inputs retain the original
sorting fallback. There is no persistent cache or additional retained storage.

The implementation commits are `2dceb26cf` (reservations), `7754ada9c`
(scheduler), `4c806d83a` (references), `b6552427b` (matrices), `e6de74c0f`
(console), and `61778df99` (gradients). Follow-up commits fix compilation of two
new tests and separate benchmark timing from allocation instrumentation.

## Measurement method

Verified on an eight-vCPU AMD EPYC Genoa VM with GCC 13.3, using optimized builds
with the default allocator and process CPU clocks. Timing binaries contain no
allocation overrides or counter bookkeeping; allocations are measured separately
in untimed instrumented runs. Timed processes were pinned to
CPU 7; engine compilation was paused. Before/after order alternated between
repetitions. The scheduler uses nine process pairs, reservations eleven pairs,
references, console and gradients nine pairs. Matrix comparisons use nine samples per revision
in alternating processes. CPU budgets are calibrated in the reservation, scheduler,
reference and console fixtures.

The fixtures compile production methods from both the baseline git revision and
the current tree. Lightweight scene/database/job adapters are explicitly documented
in their runners. The matrix fixture compiles full production math sources in
separate translation units without LTO. Correctness checks precede timing.

| Operation | Before | After | Scope |
| --- | ---: | ---: | --- |
| Reservation replacement, 1 agent | 25.64 ns | 13.55 ns | Per update |
| Reservation replacement, 1,024 agents | 1,650.74 ns | 13.03 ns | Per update |
| Reservation replacement, 8,192 agents | 13,128.86 ns | 12.79 ns | Per update |
| Scheduler, 20 declared singleton systems | 2.140 µs | 1.803 µs | Per Update + empty Draw cycle |
| Scheduler, 20 exclusive singleton systems | 2.122 µs | 1.786 µs | Per Update + empty Draw cycle |
| Reference query, 32 referrers | 13.188 µs | 6.755 µs | Per query |
| Reference query, 1,024 referrers | 1.119 ms | 0.727 ms | Per query |
| Missing reference, 1,024 indexed paths | 22.248 µs | 4.819 µs | Per query |
| Quaternion rotation matrix | 23.225 ns | 5.991 ns | Per call |
| Translate/rotate/scale matrix caller | 36.842 ns | 25.934 ns | Per call |
| Console oldest selection, 10,000 records | 7,998.78 ns | 51.43 ns | Per lookup |
| Console middle selection, 10,000 records | 3,413.09 ns | 48.72 ns | Per lookup |
| Ordered text gradient, 2 stops | 43.596 ns | 7.077 ns | Per glyph sample |
| Ordered text gradient, 8 stops | 85.743 ns | 10.528 ns | Per glyph sample |
| Ordered text gradient, 32 stops | 353.765 ns | 24.911 ns | Per glyph sample |

Scheduler allocations fall from 42 to 2 per 20-system Update + empty Draw cycle. Warmed reservation
replacements fall from one allocation to zero. The missing-reference query falls
from 1,026 to 3 allocations; the 1,024-referrer query falls from 28,756 to 3,094.
Ordered gradients fall from two allocations per glyph sample to zero.

Small controls matter: a one-referrer query measured 184.7 to 187.5 ns, approximately
1.5% slower. The console
newest-message lookup measured 2.717 to 3.022 ns, approximately 0.3 ns higher in
the newest-selection control.
Empty gradient samples measured 3.508 to 3.656 ns; reversed two/eight/32-stop
fallbacks remained within approximately 3% by median paired ratio.

## Behavior and tradeoffs

Verified differential coverage includes 30,000 reservation mutations and 7,828,192
holder/expiry queries; 12,880 reference queries with exact ordering, limits,
overlays, former GUIDs and directory boundaries; 50,000 quaternion matrices,
including non-unit inputs, signed zeros, infinities and NaNs; and console lookup
with gaps, evictions and empty/full storage. ASan/UBSan checks passed. The matrix
comparison also passed with native CPU/FMA instructions enabled.
Gradients passed 72,943 differential samples covering empty/single, ordered,
shuffled, duplicate, signed-zero and nonfinite positions/colors/parameters, also
under ASan/UBSan and native/FMA compilation. Their new allocation regression failed
on the original evaluator (128 allocations) and passed on the new one (zero).
The full real-engine suites also passed all 16 new regression cases (13 core,
three editor), including the scheduler and reservation allocation assertions.
These are separate from the standalone comparison fixtures.

The reservation map trades memory for direct lookup and storage reuse. With two
cells per agent, a separate-process glibc allocation measurement rose from 661,680
to 870,528 bytes at 8,192 agents (about 204 KiB). Active agents retain their maximum
path capacity until release/reset; erased entries disappear, while bucket capacity
can remain at its high-water mark. The previous vector also retained its outer
capacity. Inferred from the container operations: a newly seen agent still
allocates a map node, and growing a path can allocate storage. Navigation's caller
still constructs its own input path vector; zero allocations here describes
warmed replacements for existing agents, not cold insertion or the entire fixed tick.

Code review (inferred): the six cuts preserve public APIs, stored formats, ordering
and floating-point expressions. They add no persistent caches, migrations or
graphics-backend requirements and use portable CPU code. Differential tests and
the integration controls verify the cases described above; they do not establish
correctness on platforms that were not executed.

## Scene integration control

Verified three alternating before/after pairs with the actual Release editor and
its shared engine, on fresh scratch copies of `tools/fixtures/pose-animation`.
The original installation was retained before integration; its revision,
`a219eeb2d`, differs from the kernel baseline only in documentation. Each run used
Play, a fixed 640×360 Game viewport, disabled mesh LOD/depth prepass, Mesa software
GL with two renderer threads, 360 warmup frames and 60 measured frames. Compilation
was paused for all six runs.

Every run rendered the same average work: eight draws, eight instances and 6,278
triangles. Each saved exactly 60 measured profiler frames, with no crash or hang
reports. The harness also checked unchanged source-fixture hashes and recorded
both executable and shared-library hashes, commands and owned PIDs.

Median CPU-frame values were 160.382 ms before and 159.968 ms after; median overall
frame times were 183.25 and 184.06 ms. These are approximately unchanged. This
software-rendered scene verifies consistent integration/render work; it does not
establish an overall FPS gain or exercise dense reservations, reference searches,
large console histories or gradient text.

A further fresh before/after pair after the editor-shortcut and Hub upstream merge
also saved exactly 60 measured frames per run, with the same eight draws, eight
instances and 6,278 triangles and no crash/hang reports. Its CPU-frame values were
162.696 and 159.169 ms; overall frame values were 185.76 and 183.63 ms. One pair
does not establish a speed improvement.

An initial 60-warmup/180-measured control exposed existing profiler CLI behavior:
the global startup capture and benchmark capture compete, leaving no measured
capture file. The successful controls let the default 300-frame startup capture
finish before measurement and require the benchmark's own capture path and exact
frame count. This audit leaves that CLI behavior unchanged. The isolated CPU
benchmarks do not depend on it.

The full `WERROR=1 tools/build.sh linux` build succeeded, including the release
player's LTO link. That link emitted ODR diagnostics for FFmpeg/SPIR-V types in
unchanged vendored headers. Their pre-existing nature is inferred from the
unchanged files; an original-player LTO link was not run as a diagnostic control.
This audit makes no vendored-library changes.

## Verification and limits

Evidence, including raw samples, build flags, binary hashes, logs, traces,
screenshots and scratch projects, is outside the repository under
`/tmp/jasper-perf-evidence`. Verification ran on Linux and Chromium/WebGL2;
native Windows, macOS, Android and iOS were not executed in this audit.

Verified before the final mesh-update upstream merge:

- Full `WERROR=1 JOBS=4 CPUS=8 tools/build.sh unit-tests` on `5b37410eb`:
  5,752 ran (2,883 core, 2,869 editor), zero failures/errors/skips, seven existing
  disabled tests. All 16 new cases are included.
- After merging macOS watcher changes into `a7ddb921b`, the affected Linux watcher
  and animation-layer filter ran 41 tests, zero failures/skips/disabled. This does
  not test the macOS FSEvents branch.
- `WERROR=1 JOBS=2 CPUS=8 CHECK_CHANGED_VERBOSE=1 tools/check-changed --base origin/main --no-cache -j2`
  on `a7ddb921b`, with base `06d4744c7`, exited zero. Linux, unit-test and web
  builds passed. Selected unit suites ran 4,116 tests (1,692 core, 2,424 editor),
  zero failures/skips and six existing disabled tests. The MCP CI selection ran
  213 files: 1,286 passed, zero failed/skipped/crashed. The 28 files outside the CI
  selection were not run by this gate.
- Generated MCP documentation check and tool-budget check passed against the
  merged editor's 285 tools. The guide used 6,241 of 8,192 bytes. Raw-write,
  stream-key, portable-C++ and shortcut lints passed; no generated tool text changed
  in the six cuts.
- A quiet `GATE=mesh-raycast GATE_ARGS='--timeout 240' tools/build.sh web-gate`
  passed with an actual 1,280×720 screenshot, zero browser errors/exceptions,
  cross-origin isolation, and 0.080 ms per-ray p99 across 10,000 mesh rays against its 4 ms
  budget. The canonical MCP browser test passed too.

The first selected MCP CI run had 1,266 passes and two failures, zero skips or
crashes. A cold concurrent Hub compile was killed by the container's OOM killer;
the final gate used two workers and passed. The texture-importer UI test clicked
while the startup Loading modal was consuming input. The original failure was
reproduced, including with the supported 50 ms texture-load delay, and traced to
the delivered click while the modal was active. Commit `5b37410eb` adds one call
to the existing screenshot/readiness barrier before the test's clicks; assertions,
actions and limits are unchanged. Four fresh normal/delayed controls passed, as
did the final canonical gate. A runtime `settings.yaml.lock` also initially
misclassified the change as touching all platform lock files; the exact root
runtime lock is now ignored. That aborted Android attempt lacked an NDK and is
not Android validation.

The upstream mesh-update merge at `e69b8f21b` changes editor reflection and Play
snapshot restore. It leaves all seven production source files containing the six
cuts unchanged. It was integrated at `b449f7fae`; the subsequent merge
`17fe29b01` adds upstream documentation only. Verified after this merge:

- Full `WERROR=1 JOBS=4 CPUS=8 tools/build.sh linux` passed.
- `WERROR=1 JOBS=4 CPUS=8 UNIT_SUITE=editor tools/build.sh unit-tests`, with
  the filter below in `UNIT_TEST_ARGS`, passed 262 tests across 17 suites,
  zero failures/errors/skips/disabled cases. This includes the exhaustive
  update-flags oracle, unchanged/changed Mesh restoration, scene/component/material
  decoding, world transforms, undo/prefab/Play review and descriptor layouts.
- All six focused MCP files passed: `test_mesh_update_flags.py`,
  `test_reflection.py`, `test_undo.py`, `test_prefab.py`,
  `test_play_stop_gizmos.py` and `test_play_stop_overlays.py`.
  `JOBS=2 CPUS=8 MCP_SUITE=ci tools/build.sh mcp-tests -j2 <six files>` ran
  24 cases, zero failures/skips/crashed files. This includes the upstream
  600-box Stop CPU-ratio test and byte-identical saved scenes, reflected schema
  and Inspector layout, golden resaves, undo/prefab behavior and Play-stop views.
- The generated tool-documentation and tool-budget checks passed again against
  the merged editor. The earlier full gate remains evidence for its stated
  revision; these are focused post-merge checks.

```sh
WERROR=1 JOBS=4 CPUS=8 UNIT_SUITE=editor \
UNIT_TEST_ARGS='--gtest_filter=ChangedUpdateFlags*.*:StreamScene.*:*/StreamFixtureScene.*:StreamComponent.*:StreamMaterialPbr.*:WorldTransforms.*:UndoProjectTest.*:PrefabUndoTests.*:PlayChangeReviewTests.*:ReflectRegistry.*:ReflectParity.*:ReflectFixtures.*:ClipPlayersReflect.*:CanvasGroup*.*:TextEffectStreamTest.*' \
tools/build.sh unit-tests
```

Later ledger-only upstream commits through `4b3afb124` were reviewed and merged;
they change no executable source. No migrations or API changes are needed for the
six cuts. The retained-memory tradeoff and profiler CLI limitation described above
remain; no overall FPS improvement or unexecuted native-platform result is claimed.

## Reproduction

Use an otherwise idle Linux machine with Python 3 and a C++20 GCC-compatible
compiler. Set an output path outside the repository to keep evidence separate:

```sh
python3 tools/performance/run-grid-reservations.py --baseline-ref 08dea5e0e --output /tmp/jasper-perf/grid
python3 tools/performance/run-scheduler.py --baseline-ref 08dea5e0e --output /tmp/jasper-perf/scheduler
python3 tools/performance/run-asset-references.py --baseline-ref 08dea5e0e --output /tmp/jasper-perf/references
python3 tools/performance/run-quaternion-matrix.py --baseline-ref 08dea5e0e --output /tmp/jasper-perf/matrix
python3 tools/performance/console.py --baseline-ref 08dea5e0e --selection > /tmp/jasper-console-perf.csv
python3 tools/performance/run-text-gradient.py --baseline-ref 08dea5e0e --output /tmp/jasper-perf/gradient
```

Each runner supports `--sanitize`. The matrix and gradient runners also support
`--native`; gradients support `--allocations` for separate allocation instrumentation.
The scheduler, reservation and reference runners also use `--allocations` for
separate untimed counts; reference allocation regression checks additionally need
`--check-cuts`. Their initial allocator-instrumented CPU samples are archived;
the table above uses the fresh default-allocator controls.
Reservation and scheduler allocation regressions fail against the original
implementation and pass with the cut. The reference runner's `--check-cuts`
assertion likewise failed with the original query and passed afterward. Quaternion
characterization tests pass on both revisions; a deliberate transposed-constructor
mutation failed two of three tests, validating the layout/signed-zero checks.
The old timings are the performance controls for behavior-preserving matrix and
console changes; their functional tests are expected to pass on both revisions.
