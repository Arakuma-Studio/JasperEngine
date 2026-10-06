# Performance audit

This audit targets repeated CPU work in the native engine, editor, and MCP tools. Measurements compare production code at `0da9f6ab81b25880ef6b51341f80c00eabead4b0` with the changes in this commit. The benchmarks isolate the affected operations; their speedups do not represent whole-frame FPS improvements.

## Critical paths

- `Engine::systemDraw` advances fixed-step physics and variable-step scene systems, then prepares and draws each scene. Instance-view construction repeats for camera and shadow views.
- Runtime GameObject name and tag queries scan the entity registry. Scripts can repeat these queries each update.
- Editor console ingestion retains a bounded history, updates severity counters, and rebuilds its filtered view when needed. Log bursts exercise eviction on every append after the history fills.
- Entity references from the CLI and MCP resolve names and hierarchy paths. Scene-tree requests also assemble ordered parent and child lists.
- Text input counts UTF-8 codepoints and converts text for rendering. The integrated code editor draws search highlights on every visible frame.

## Prioritized opportunities

| Priority | Opportunity | Expected impact and risk |
| --- | --- | --- |
| 1 | Repeated sibling scans during entity-path resolution | Remove quadratic work in large scenes while preserving duplicate-name suffixes and Transform order. |
| 2 | Console eviction shifts and rescans the retained history | Make bounded-history append independent of retained message count; preserve line trimming, counters, and selection. |
| 3 | Instance views sort data already in insertion order | Remove sorting and temporary candidates for KEEP order; retain culling and other ordering modes. |
| 4 | Runtime entity lookup copies snapshots and metadata | Scan existing ordered metadata directly; preserve first-match and parent-filter semantics. |
| 5 | MCP hierarchy construction uses ordered membership indexes | Use membership indexes without ordering costs; retain output order in the existing vectors. |

UTF-8 temporary buffers and off-screen search highlights are additional low-risk opportunities evaluated with dedicated benchmarks.

## Measurements and verification

Measurements ran on Linux x86-64, an AMD EPYC host with 12 logical CPUs, using GCC 13.3. Benchmarks use optimized C++17 builds and median process CPU time over repeated samples. The host was shared with other work, so small differences are noise. The editor integration build uses Release (`-O3 -DNDEBUG`).

| Production operation and fixture | Before | After | Improvement |
| --- | ---: | ---: | ---: |
| Entity-path last-match lookup, 5,000 uniquely named roots | 801.872 ms | 3.025 ms | 265× |
| Entity-path last-match lookup, 5,000 duplicate-named roots | 774.870 ms | 2.033 ms | 381× |
| Console append, 160-byte message into a full 10,000-line history | 46.19 µs | 0.280 µs | 165× |
| Instance-view preparation, 10,000 KEEP instances, culling disabled | 669.79 µs | 51.19 µs | 13.08× |
| Instance-view preparation, 100,000 KEEP instances, culling enabled | 8.255 ms | 7.138 ms | 1.16× |
| Registry last-name lookup, 1,000 entities with short names | 39.52 µs | 7.55 µs | 5.23× |
| Registry all-tag lookup, 1,000 entities with the default tag | 48.00 µs | 9.01 µs | 5.33× |
| MCP hierarchy assembly, 100,000 nested authored entities | 224.58 ms | 51.97 ms | 4.32× |
| MCP JSON traversal, 100,000 entities in a wide tree | 345.51 ms | 193.04 ms | 1.79× |
| Code-editor highlight traversal, 100,000 matches, small visible range | 71.236 µs | 0.368 µs | 194× |
| UTF-8 codepoint count, 1,024-byte ASCII input | 1,491.58 ns | 600.51 ns | 2.48× |

Console append below capacity was essentially unchanged. NEAREST and BACK_TO_FRONT instance-view controls were approximately unchanged; the KEEP optimization only helps when those views are rebuilt. The measured registry name query removes 11 allocations per call; all-tag lookup drops from 22 to 11. Counting UTF-8 codepoints removes one allocation of four bytes per input byte. Wide-string conversion keeps its previous output-capacity behavior and has no claimed performance gain.

Entity-path lookup retains direct scanning for candidate lists smaller than 16 and for the first 16 Transform positions. This avoids building a whole-prefix index for a late single-candidate query. Three interleaved runs of the 10-entity unique-name cases still measured 2.214 → 2.773 µs for a last match and 2.179 → 2.567 µs for a miss; first-root matches changed from 0.132 to 0.153 µs. The large-scene improvement trades this small-input overhead and extra memory for fewer repeated scans. Its per-call sibling index uses memory proportional to the visited Transform prefix. There is no persistent cache or invalidation dependency. Add `--subsets` to the entity-path benchmark to check late candidates in a large scene.

Public API and output behavior are retained. In particular, entity results retain ID and Transform ordering, path suffixes include siblings outside the queried entity subset, the console keeps the same line limit and partial-message trimming, and instance counts above `2^24` retain the original float-key sort to preserve tie ordering. The path-index memory and small-input costs above are the measured performance tradeoff.

The harnesses compile the production methods or translation units. Lightweight fixtures replace Scene/registry ownership, tag/parent predicates, or drawing surfaces where necessary to avoid graphics startup. Instance tests link real engine math; entity-path tests use the real EntityManager, ComponentArray, and Transform. These tests measure CPU operations, not GPU time, whole-frame FPS, or complete MCP request latency.

Reproduce on Linux with Python 3 and a C++17 compiler, from the repository root:

```bash
BASELINE=0da9f6ab81b25880ef6b51341f80c00eabead4b0
python3 tools/performance/run-entity-path.py --baseline-ref "$BASELINE"
python3 tools/performance/console.py --baseline-ref "$BASELINE"
python3 tools/performance/run_instance_views.py --baseline-ref "$BASELINE"
python3 tools/performance/run-entity-lookup.py --baseline-ref "$BASELINE"
python3 tools/performance/mcp-hierarchy.py --baseline-ref "$BASELINE"
python3 tools/performance/text_search.py --baseline-ref "$BASELINE"
python3 tools/performance/run-string-utils.py --baseline "$BASELINE"
python3 tools/performance/run-string-utils.py --baseline "$BASELINE" --allocations
```

The dedicated correctness fixtures compare old and new results, including randomized hierarchy text and JSON, entity mutations and duplicate matches, malformed UTF-8, console rollover and filtering, visible highlight rectangles, and 480 instance-view cases. These checks passed with AddressSanitizer and UndefinedBehaviorSanitizer. For sanitizer reproduction, add `--sanitize` to the path, registry, instance-view, and MCP commands; add `--sanitize --check-only` to console and text-search commands; use `--check` for StringUtils. UTF-8 checks cover every Unicode scalar encoding, all 65,536 two-byte combinations, and 10,000 random byte strings. Linux executes the native wide-character branch; the UTF-16 branch is unchanged.

The stable-source Linux Release editor and standalone sample build passed with `JOBS=4 tools/build.sh linux`. Independent MCP stdio/HTTP smoke checks passed, including schema validation for all 129 tools, session handling, SSE, undo/redo, and grouped batches. The smoke test's stale project-summary assertion was corrected to check the existing `scenes_in_build` field; the production API did not change.

The tracked `tools/fixtures/runtime-shaders` project passed a live Play smoke test: instanced, skinned, custom, depth, and fog shader variants compiled with no failures or pending work. A captured Game view showed the scene geometry and all five instanced rings. This is rendering correctness evidence, not a GPU performance measurement.

The full live MCP suite uses a preexisting local TappyPlane project at `build/performance/integration/tappy`. That fixture is ignored and is not distributed with this patch; a compatible TappyPlane fixture is needed to reproduce that suite. Its command is:

```bash
JASPER_FIXTURE="$PWD/build/performance/integration/tappy" tools/mcp/run-tests.sh -j 3
```

The final run completed with **156 passed, 0 failed, and 3 skipped in 641.2 seconds**. The skips are opt-in desktop build/player tests (two) and a C++ build-error Play test (one). The ordinary C++ script compilation test did run and pass. The suite also exercised scene save/reload, hierarchy queries, UI, physics initialization, screenshot capture, and concurrent transport stress. Logs and the runtime-shader screenshot are retained under `build/performance/integration/` in this workspace.

## Deferred changes

Animation keyframe lookup was benchmarked with `std::find_if`; it showed no consistent improvement and was reverted. Binary search would change behavior for mutable, unsorted keyframe times. Skipping sleeping physics bodies can miss parent-transform reconciliation. Persistent entity-path caches and asset-path canonicalization caches need reliable invalidation. Full-document undo snapshots require a separate behavioral design. These changes are outside this patch.
