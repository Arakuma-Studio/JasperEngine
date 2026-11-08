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

To be filled from the run named in the landing report.

## Size (R54)

`tools/perf/terrain_size.py` compares a Linux glcore player's `.text` + `.rodata` and the web `st` player's
gzipped `.wasm` between the merge base and the head. Budgets: 96 KiB and 32 KiB.
