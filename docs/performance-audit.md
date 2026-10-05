# Performance audit October 2026

This audit targets native editor interaction, project loading, and engine CPU work. The baseline is commit `6479bcb0`. Measurements apply to the operations below, not to overall editor FPS. No public API or file-format changes are intended.

## Critical paths

- `Engine::systemDraw` drains resource uploads and shader compilation, runs fixed and frame updates, processes deferred destruction, draws scenes, and commits the frame.
- `Scene::update` runs transforms/render preparation, mesh/UI systems, action animation, and audio; Lua/C++ callbacks also run on the frame path.
- Editor hierarchy display derives search flags and range-selection order from a tree that is already cached. Inspector and project windows add work to the same interactive frame.
- Opening/rescanning a project reads prefab GUIDs through `AssetDatabase`. Scene serialization and shader loading decode binary payloads with `Base64`.
- Hierarchy reordering calls `sortComponentsByTransform`; transparent instance preparation sorts an index permutation and moves the matching instance bounds with it.

## Five ranked opportunities

Ranking reflects expected impact and implementation risk from source inspection, before benchmark results.

| Rank | Opportunity | Expected impact and chosen change |
| --- | --- | --- |
| 1 | Hierarchy search and selection preparation every frame | Large hierarchies repeat linear traversal and name processing. Reuse derived results until the cached tree, query, or case setting changes. |
| 2 | Entire prefab read for a top-level GUID | Large prefab projects read and copy asset bodies unnecessarily. Stream lines and return at the first matching GUID. |
| 3 | Instance sort recomputes projected depth in every comparison | Replace two model projections per comparison with one per instance, keeping the same index sort and gather logic. |
| 4 | Base64 alphabet search for every input byte | Replace repeated alphabet searches with a constexpr lookup table in the production decoder. Retain acceptance, padding, truncation, and locale behavior. |
| 5 | ECS and animation temporary allocations | Detect already-sorted component keys before allocating the permutation; bind previous morph weights by const reference instead of copying them each update. |

## Measurement method

The host is Linux x86-64 on an AMD EPYC virtual CPU, using GCC 13 and C++17. It was heavily oversubscribed during testing. Benchmarks therefore use process or thread CPU time, retain wall time where available, use repeated samples, verify outputs, and make no end-to-end FPS claim. Hardware profiling was unavailable (`perf_event_paranoid=4`). The editor has an existing `benchmark` CLI for scene-level smoke checks.

The component and asset harnesses compile production code directly. The instance harness calls the production helper with real engine matrices and instance types, comparing it to the original comparator. The hierarchy and morph harnesses extract production methods into minimal fixtures without initializing graphics/audio. These isolate the changed work; they do not substitute for the full editor build and integration checks.

## Measured results

Median CPU time in release builds:

| Measured operation | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Hierarchy matching search, 10,000 leaves, per frame | 2.227 ms | 0.070 ms | 96.9% |
| Hierarchy matching search, 100,000 leaves, per frame | 25.850 ms | 0.858 ms | 96.7% |
| GUID scan, 64 × 1 MiB prefabs, GUID first | 37.992 ms | 0.478 ms | 98.7% |
| GUID scan, 64 × 1 MiB prefabs, GUID last | 71.417 ms | 64.942 ms | 9.1% |
| Instance depth ordering, 10,000 instances | 15.551 ms | 1.159 ms | 92.5% |
| Instance depth ordering, 100,000 instances | 238.382 ms | 14.195 ms | 94.0% |
| Already-sorted component array, 10,000 entries | 0.102 ms | 0.050 ms | 50.7% |
| Already-sorted component array, 100,000 entries | 2.669 ms | 0.816 ms | 69.4% |
| Morph sampling, 8 weights, linear, per call | 41.1 ns | 16.4 ns | 60.1% |
| Morph sampling, 8 weights, cubic, per call | 39.2 ns | 20.4 ns | 48.0% |
| Base64 decode, 1 MiB decoded payload | 27.209 ms | 5.423 ms | 80.1% |
| Base64 decode, 8 MiB decoded payload | 227.689 ms | 43.264 ms | 81.0% |

Hierarchy timings include four periodic refreshes over 120 frames, with three samples. Asset and Base64 results also use seven-sample medians. A 4 KiB GUID-first scan improved only slightly (0.596 to 0.533 ms), as expected when file-open overhead dominates. The unchanged Base64 encoder was retained as a control; its measured CPU times varied by 4–8%, much less than the decoder improvement.

Already-sorted component arrays now allocate one scratch vector instead of two. A separate 14-sample alternating comparison in one process checked reverse-ordered inputs: 19.418 to 18.268 µs at 1,000 entries, 339.825 to 334.455 µs at 10,000, and 4.720 to 4.510 ms at 100,000. These small differences are treated as no material regression, not as additional speed claims. An earlier implementation that checked monotonicity inside every key lookup was rejected after adverse measurements.

Morph sampling removes one heap allocation per valid call. Instance timing uses random finite positions and includes key/index allocation and sorting; the unchanged gather/scatter phase and GPU rendering are excluded.

Raw outputs are kept under ignored `build/performance/` and `build/performance-core/`; the committed harnesses and baseline reference reproduce them.

## Reproduction

Run from the repository root with a C++17 compiler and Python 3:

```bash
python3 tools/performance/hierarchy.py --baseline-ref 6479bcb0
python3 tools/performance/run_core.py --baseline-ref 6479bcb0
python3 tools/performance/run_core.py --baseline-ref 6479bcb0 --only component --paired-component --output build/performance-core/repro-paired
python3 tools/performance/run_core.py --baseline-ref 6479bcb0 --sanitize --output build/performance-core/sanitized
python3 tools/performance/run-base64.py --baseline 6479bcb0
python3 tools/performance/run-base64.py --baseline 6479bcb0 --check
bash tools/performance/instance-sort.sh
mkdir -p build/performance/assets
git show 6479bcb0:editor/util/AssetDatabase.cpp > build/performance/assets/AssetDatabase.before.cpp
ASSET_GUID_BENCH_OUT=build/performance/asset-guid-before bash tools/performance/asset_guid_benchmark.sh build/performance/assets/AssetDatabase.before.cpp
ASSET_GUID_BENCH_OUT=build/performance/asset-guid-after bash tools/performance/asset_guid_benchmark.sh
```

The scripts fail on compatibility mismatches. Timings should be collected on a quiet host when comparing small differences. The asset benchmark uses a warm filesystem cache and synthetic prefab bodies; it does not claim physical disk throughput or total project-open latency.

## Validation

- Exact baseline editor built in an isolated checkout at `6479bcb0`; the changed Linux Release editor and standalone engine also built successfully.
- All three existing prefab suites (`prefab-instance-edit-test.py`, `prefab-nested-test.py`, `prefab-variant-test.py`) passed on both builds. The prefab-runtime scene dumps were byte-for-byte identical.
- The standalone Linux sample stayed alive with its expected 960 × 540 JasperEngine window; its line-buffered runtime log contained no error markers.
- Both editors completed the TappyPlane GUI smoke workload: 30 measured frames after 10 warmup frames, a 753 × 384 viewport, two draws, two instances, and four triangles. The timing was not used to claim an overall FPS improvement.
- Targeted compatibility checks passed: 252 exact instance orders, 2,016 GUID parser cases plus missing files, Base64 round trips and malformed-byte inputs, hierarchy invalidation/selection ordering, component permutations/index mappings, and morph interpolation/boundary cases.
- ASan/UBSan checks passed for Base64 before and after, and for the final component and morph fixtures. Python/shell syntax checks and `git diff --check` passed.

Integration logs are under `build/performance/integration/`; the exact baseline logs and binaries remain in `/tmp/jasper-baseline-6479/build/`. Reproduce the native build with `JOBS=4 tools/build.sh linux`. The existing prefab suites require their TappyPlane sprite fixture via `FIXTURE`; this run used `build/audit/perf-smoke-source`. Full platform validation here is Linux; Windows, macOS, Android, and Web were not built during this audit.

## Behavior and tradeoffs

- Hierarchy caching follows the existing tree refresh rules, including its half-second refresh. It does not add delay to existing rename/structure updates or change range selection ordering.
- Instance sorting uses the same floating-point expression and comparator ordering. It adds four bytes of temporary depth storage per instance, released before the existing gather allocations.
- The GUID reader retains first column-zero match, quotes/comments, CRLF, malformed-input and missing-file handling. Files whose GUID is late or absent still require scanning the file.
- Base64 changes only alphabet lookup. It retains the existing locale-sensitive validity check, partial-group behavior, and allocation strategy.
- Component sorting retains the original sorting/permutation logic for unordered input. Morph animation retains interpolation and blending math.

## Deferred opportunities

Audio source updates may repeatedly invoke SoLoud's whole-voice 3D update, and script callback guards install/remove signal handlers repeatedly. Both need dedicated lifecycle/threading verification before changing them. Keyframe search, direct transform/AABB math, hierarchy selection-map refresh, and text shaping also merit separate measurements; their ordering, numeric, or invalidation risks exceed the selected changes. Third-party code was not modified.
