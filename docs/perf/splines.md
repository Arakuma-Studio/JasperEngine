# Spline performance

The budgets of [S18](../specs/S18-splines.md) (R18-R22, AC16, AC17), how they are measured and
what the benchmark server measured. The core budgets are `je-bench-splines` (built next to the
core tests); `tools/perf/check_bench.py` gates its JSON with `tools/perf/bench-thresholds.json`.
The terrain bake is editor code, so its budgets (R20) are an editor test.

```sh
cmake --build build/linux/core-tests --target je-bench-splines
build/linux/core-tests/tests/core/je-bench-splines --count-allocs --json splines.json
python3 tools/perf/check_bench.py splines.json
# R20, enforced when JASPER_TEST_PERF_SLACK is set (1 = the budgets as written)
JASPER_TEST_PERF_SLACK=1 UNIT_SUITE=editor UNIT_TEST_ARGS='--gtest_filter=TerrainSplineBake*' tools/build.sh unit-tests
```

## What it measures

Every figure is the median of repeated runs on one thread (Release, `-ffp-contract=off
-fno-math-errno` on the spline sources).

- **R18** on a 100-knot Catmull-Rom meander 1 km long: `evaluateAtDistance` (a full frame:
  position, forward, right, up, radius, roll) over 200 000 distances made beforehand, with its two
  halves reported beside it (`distance_to_t_ns`: the arc-table lookup and the Hermite inversion;
  `evaluate_t_ns`: the frame at a parameter); `closestPoint` of 20 000 points near the curve; the
  cache rebuild (`markDirty` + `ensureCache`) and the cache's bytes against R22's 4 KB + 64 B per
  knot.
- **R19**: a 1 km Crown road (OpenEngine's 5 Crown points, the 2 shoulder creases split, so 7
  vertices a ring), stations at most 1 m apart, from the stations to every chunk's vertices; 2 000
  pieces laid by the tile layout (Spacing, 2 m along 4 km, yaw jitter), the centreline included;
  a 300 m FitToBanks river on a 1025 x 1025 ground lattice (the field and every chunk's mesh).
- **R20** (`TerrainSplineBakeTests.BakeBudgets`): a 1 km road baked into a 1025 x 1025 16-bit
  heightmap and one blend map (heights, flatten, paint), and a Live Terrain re-bake after a 50 m
  drag of one knot (the touched texels restored, then baked).
- **R21**: 10 000 frames of `SplineSystem::update` in a scene without spline components (mean)
  and the allocations they make (`--count-allocs`, the S06 counting allocator); the cost a Spline
  Animate follower adds to a frame (1 000 followers on one 200 m spline against none, median of
  200 frames, divided by 1 000).

## Benchmark server (E: 4 dedicated vCPU, Linux x64 Release, GCC 11)

Five runs of `je-bench-splines --count-allocs`, every run within every budget:

| Metric | Budget | Runs (5) | Median |
|---|---|---|---|
| `evaluate_at_distance_ns` | <= 60 | 54.95 - 57.65 | 55.2 |
| (`distance_to_t_ns`, info) | | | 26.5 |
| (`evaluate_t_ns`, info) | | | 24.1 |
| `closest_point_100_knots_us` | <= 6 | 1.12 - 1.18 | 1.17 |
| `cache_rebuild_100_knots_us` | <= 60 | 26.9 - 27.2 | 27.1 |
| `cache_bytes_100_knots` | <= 10 496 | 10 424 | 10 424 |
| `extrude_crown_1km_ms` | <= 3 | 0.518 - 0.527 | 0.520 |
| `crown_ring_vertices` | 7 | 7 | 7 |
| `instantiate_2000_ms` | <= 2 | 0.587 - 0.592 | 0.590 |
| `fit_to_banks_300m_1025_ms` | <= 25 | 1.53 - 1.56 | 1.53 |
| `empty_scene_frame_us` | <= 1 | 0.0035 - 0.0042 | 0.0035 |
| `empty_scene_allocations` | 0 | 0 | 0 |
| `spline_animate_follower_us` | <= 0.5 | 0.321 - 0.327 | 0.322 |

`TerrainSplineBakeTests.BakeBudgets` with `JASPER_TEST_PERF_SLACK=1` (median of its runs):

| Metric | Budget | E |
|---|---|---|
| Bake of a 1 km road into a 1025^2 heightmap + 1 blend map | <= 150 ms | 46.4 ms |
| Live Terrain re-bake after a 50 m knot drag | <= 16 ms | 0.60 ms |

R18's `evaluateAtDistance` was 64 ns before its fast path: the cache's up-to-date check is inline,
the arc lookup starts at the distance's share of the table (`arcEntriesPerUnit`) and walks at most
four entries before a binary search, the segment index needs no integer division, the located
segment is reused for the frame, and forward and right are normalized side by side. The results
are bit-identical to before (the road fixture's vertex hash, R16, is unchanged).

## Web (R17, AC17)

`tools/test-projects/splines` (`WEB_PROJECT=splines tools/build.sh web`, `GATE=splines
tools/build.sh web-gate`): a 1.2 km Crown road rebuilt after a Lua `setKnotPosition`, without
pthreads. The chunks are Background jobs the engine pumps for at most 4 ms a frame; the test
project logs an error line when a frame's spline work (the S15 CPU scope) passes 4 ms or the
rebuild takes more than 10 frames, and the gate fails on it.
