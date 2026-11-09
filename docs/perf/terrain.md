# Terrain engine: budgets and measured numbers

T1 R47-R54 (`docs/specs/T1-terrain-authoring.md`). The CPU budgets are `je-bench-terrain` metrics
(`engine/tests/core/bench/TerrainBench.cpp`), gated by `tools/perf/check_bench.py` against
`tools/perf/bench-thresholds.json`; R49's delta step lives in the editor, so `TerrainDeltaBench`
(`editor/tests/terrain/TerrainDeltaBenchTest.cpp`) times it and enforces its budgets only under
`JASPER_TEST_PERF_SLACK`. The spec states the budgets for an idle 8-core desktop in release. No number
below is from such a machine: see "Measured".

What each metric runs:

| Metric | Requirement | What runs |
|---|---|---|
| `stamp_raise_r64_ms`, `stamp_smooth_r64_ms`, `stamp_smooth_slope_filter_r64_ms` | R47 | One brush stamp of radius 64 texels on a 1024² 16-bit map, as `TerrainStrokeSession` runs it (in place on the stroke's overlay, a click's deposit), median over 200 stamps at R33's spacing; tile copies on first touch included |
| `stamp_raise_r512_ms` | R47 | The same at radius 512 on 2048², on the job system's workers (`workers` is reported) |
| `set_height_4096_ms` | R48 | `applyOps` `set_height` over the whole of a 4096² map, committed |
| `paint_by_rules_3_2048_ms` | R48 | Three rules (ground, slope, height with noise) over 2048², repainting an existing blend map |
| `overlay_commit_64_tiles_ms` | R48 | `OverlayMapStore::commit` of 64 changed tiles |
| `sample_height_ns`, `sample_height_normal_ns`, `raycast_512m_us` | R50 | `sampleHeight`, `sampleSurface` and `raycastSurface` over 1024² on 1024 units, 2^20 points and 4096 rays of 512 units |
| `*_vs_today` | R50 | The same against a copy of the code the surface query replaced (`MeshSystem` before `40d74d630`), interleaved in the same run, best of 5 rounds each |
| `node_refit_64_ms`, `node_full_rebuild_ms` | R51 | `MeshSystem::refitTerrainNodes` of a 64×64 rectangle on 2 roots, 6 levels, over 1024²; the full rebuild (`needUpdateTerrain`) next to it, unbudgeted |
| `runtime_set_heights_64_ms`, `runtime_apply_circle_r32_ms` | R52 | A game's `setHeights` of 64×64 and `applyOps` circle of radius 32 texels on the component's store, main thread, the frame's flush between calls not timed |
| `collision_refresh_64_ms` | R52 | `JoltTerrainRefresh::refreshShape` of a 64×64 sample block of a 1025-sample heightfield; `collision_refresh_64_rebuilds` counts refreshes that fell back to a rebuild |
| `TerrainDeltaBench.CommitAndUndoWithinBudget` | R49 | A stroke's end cut into a `TerrainMapDeltaCmd` and undone: 64 tiles, and a whole 4096² map (1024² without the slack) |
| `TerrainDeltaBench.ADiagonalStrokeStoresAnEighthOfTodaysPatch` | R49 | A count, asserted on every run |

R52's upload size per frame is exact, not timed: `TerrainEditGpu.PacingUploadsAtLeastTheLast` (AC36) pins it.
R53 is `TerrainEdit.ZeroCostWhenUnused` and `TerrainOverlay.OffHasNoEntities`.

## Measured

`je-bench-terrain` before and after T1's budget work, on one tree and one box: the before binary from
`terrain/integration` at d7e17a641, the after one from `terrain/t1-perf` with it merged, both RelWithDebInfo
(the core-tests tree of `tools/build.sh unit-tests`), run alternately three times each on the programme's rented
build box: 16 shared vCPUs (15 job workers), another stream's jobs able to share it. The medians of the three
are below. That is neither the spec's idle 8-core desktop nor an idle machine, so a missed budget here is not
shown to be missed on reference hardware, and a met one is not shown to be met there;
`docs/specs/checklists/T1.md` holds the reference run. Every result is bit-identical to before: the golden
digests (`TerrainGolden.Hashes`) did not change.

| Metric | Budget | Before | After |
|---|---|---|---|
| Raise stamp, radius 64 | 0.3 ms | 0.415 ms | 0.287 ms |
| Smooth stamp, radius 64 | 0.8 ms | 0.545 ms | 0.496 ms |
| Smooth stamp with the slope filter | 1.0 ms | 1.156 ms | 0.779 ms |
| Raise stamp, radius 512, workers | 8 ms | 14.69 ms | 6.50 ms |
| `set_height` on 4096² | 400 ms | 829 ms | 222 ms |
| `paint_by_rules`, 3 rules, 2048² | 600 ms | 1292 ms | 225 ms |
| Overlay commit, 64 tiles | 1 ms | 3.27 ms | 0.388 ms |
| Height query | 40 ns, 1.05x | 24.4 ns, 1.094x | 21.6 ns, 0.982x |
| Height and normal | 150 ns | 77.1 ns, 1.043x | 74.1 ns, 0.999x |
| Raycast, 512 m of 1024² | 30 us | 3.68 us, 1.030x | 3.58 us, 1.004x (the same 3668 hits of 4096) |
| Node refit, 64×64 | 0.1 ms | 0.030 ms | 0.033 ms (a full rebuild: 5.0 ms) |
| Runtime `setHeights` 64×64 | 0.2 ms | 0.554 ms | 0.124 ms |
| Runtime `applyOps` circle radius 32 | 0.2 ms | 0.624 ms | 0.177 ms |
| Collider refresh, 64×64 samples | 2 ms | 0.539 ms | 0.530 ms, no rebuild |

What changed, from profiles of the before code: a whole-map op encoded every overlay tile four times (the
report, its statistics, the commit and the flush) and grew dirty rectangles texel by texel; now a tile's after
bytes are encoded once, over the texels written, and the report, commit and flush scan and encode tiles on the
workers. The overlay's writes check, decode and store tiles on the workers too, the rule filter's rows run on
them, and a large raw read decodes rows on them. A stamp's block no longer zeroes buffers it fills, and samples
its shape a row at a time. The height query reads a 16-bit map's texels directly.

The radius-512 stamp, the height query and the runtime writes come from a second pass, measured the same
way with the box's other build slot held, five interleaved runs against the first pass's code. The stamp's
two 4 MB block buffers were fresh pages each call; a stroke now keeps them (`OpOptions::scratch`) and frees
them with itself. The height query's NaN test of the position cost it about 8 %: the clamp it already does
is now the guard. The reports compared each texel with a `memcmp` call; a 16-bit texel is compared in place.
Every `je-bench-terrain` budget is met on the box.

`TerrainDeltaBench` with `JASPER_TEST_PERF_SLACK=1` (the spec's sizes), three runs on the same box: commit of
64 tiles 0.39 to 0.50 ms (budget 2; 1.09 to 1.11 ms before the commit packed tiles on the workers), undo of
64 tiles 0.56 to 0.59 ms with one run at 1.09 ms (budget 1; the undo is unchanged; the second pass's five
runs with the box to itself: 0.555 to 0.592 ms), commit of a whole 4096² map
10.9 to 12.6 ms (budget 60; 68.8 to 71.5 ms before), its undo 38.6 to 39.3 ms (budget 60). A radius-64
diagonal stroke on 2048² stores 67,026 bytes in 154 tiles against today's 8,388,608-byte rectangle: 1/125,
inside R49's 1/8.

## Size (R54)

`tools/perf/terrain_size.py` compares a Linux glcore player's `.text` + `.rodata` and the web `st` player's
gzipped `.wasm` between two players, and with `--t1-share` counts T1's own sources (the operation engine,
stores, surface query, op JSON, Lua terrain bindings, collider refresh, refit) in a player's debug file by
their symbols' line information. Budgets: 96 KiB and 32 KiB.

The runtime edit path (`applyOps`, `setHeights`, `getHeights`, the overlay store, strokes, placement, erosion
and T5's registered foliage ops, the edit uploads and the collider refresh) is the player module `terrainedit`
(`JASPERENGINE_TERRAIN_EDIT`): a project needs it when a terrain has Allow Runtime Edits or a script calls one
of those functions, and the `lite3d` and `lite2d` variants leave it out. Measured with
`tools/players/build_linux_player.sh`, release, warnings as errors, on the merged tree:

| Player | T1's share |
|---|---|
| Without runtime edits (`full` less `terrainedit`) | 40.1 KiB, inside the 96 KiB |
| With them (`full`) | 320.9 KiB, of which `TerrainOps` 170.3 KiB |

The module adds 456.0 KiB to the Linux glcore player's `.text` + `.rodata` (38,331,954 bytes against
37,864,994), more than T1's share because it also keeps the code it calls in other modules' files. Nothing
in the module was shrunk: its size is the operation engine's per-op kernels, each a template instance per
map format. The web `st` player was not built: web builds run once at the end of the programme.
