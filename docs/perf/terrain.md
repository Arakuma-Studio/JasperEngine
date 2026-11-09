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

One run of `je-bench-terrain` (commit 9714f837d, RelWithDebInfo, the core-tests tree of `tools/build.sh unit-tests`)
on the programme's rented build box: 16 shared vCPUs (15 job workers), a second build slot open to another stream.
That is neither the spec's idle 8-core desktop nor an idle machine, so a missed budget here is not shown to be
missed on reference hardware, and a met one is not shown to be met there; `docs/specs/checklists/T1.md` holds the
reference run.

| Metric | Budget | This run |
|---|---|---|
| Raise stamp, radius 64 | 0.3 ms | 0.41 ms (over) |
| Smooth stamp, radius 64 | 0.8 ms | 0.52 ms |
| Smooth stamp with the slope filter | 1.0 ms | 1.12 ms (over) |
| Raise stamp, radius 512, workers | 8 ms | 13.9 ms (over) |
| `set_height` on 4096² | 400 ms | 798 ms (over) |
| `paint_by_rules`, 3 rules, 2048² | 600 ms | 1295 ms (over) |
| Overlay commit, 64 tiles | 1 ms | 4.3 ms (over) |
| Height query | 40 ns | 23.6 ns (the old code 21.8 ns: 1.08x, over the 1.05x bound) |
| Height and normal | 150 ns | 76.0 ns (the old code 73.6 ns: 1.03x) |
| Raycast, 512 m of 1024² | 30 us | 3.65 us (the old code 3.61 us: 1.01x, the same 3668 hits of 4096) |
| Node refit, 64×64 | 0.1 ms | 0.032 ms (a full rebuild: 4.9 ms) |
| Runtime `setHeights` 64×64 | 0.2 ms | 0.57 ms (over) |
| Runtime `applyOps` circle radius 32 | 0.2 ms | 0.63 ms (over) |
| Collider refresh, 64×64 samples | 2 ms | 0.57 ms, no rebuild |

`TerrainDeltaBench` in the same pass, without the slack (1024² for the whole map): commit of 64 tiles 2.19 ms
(budget 2), undo of 64 tiles 1.23 ms (budget 1), commit of a whole 1024² map 9.0 ms and its undo 5.0 ms (the
budget's 4096² was not run). A radius-64 diagonal stroke on 2048² stores 67,026 bytes in 154 tiles against
today's 8,388,608-byte rectangle: 1/125, inside R49's 1/8.

The ratios are the most trustworthy lines: both sides run interleaved in one process. The whole-map ops and the
overlay commit miss by 2x to 4x, more than a shared machine explains; they are the place to look first.

## Size (R54)

`tools/perf/terrain_size.py` compares a Linux glcore player's `.text` + `.rodata` and the web `st` player's
gzipped `.wasm` between the merge base and the head. Budgets: 96 KiB and 32 KiB.

Measured once (the release `full` variant through `tools/players/build_linux_player.sh`): from the merge base
with `origin/main` (1aa6d41f7) to 9714f837d the Linux glcore player grew by 1,673 KiB of `.text` + `.rodata`.
That is the whole terrain programme in the head (T1, T2's noise module, T3, T5, T6's voxel module), so it
bounds T1's growth from above. T1's own sources (the operation engine, stores, surface query, op JSON, Lua
terrain bindings, collider refresh, refit, X11 pen reader), counted by their symbols' line information in the
head's debug file, hold 319,172 bytes: about 312 KiB, over R54's 96 KiB. `TerrainOps*` alone is 164,535 bytes.
The web `st` player was not built: web builds run once at the end of the programme.
