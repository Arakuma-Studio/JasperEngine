# Navigation performance

The budgets of [specs/S19-navigation.md](../specs/S19-navigation.md) R25-R30 and what the
benchmark server measured. The benchmark is `je-bench-nav` (built next to the core tests,
`engine/tests/core/bench/NavBench.cpp`):

```sh
cmake --build build/linux/core-tests --target je-bench-nav
build/linux/core-tests/tests/core/je-bench-nav --count-allocs --json nav.json [--workers N]
python3 tools/perf/check_bench.py nav.json
```

Its thresholds are the `je-bench-nav` entry of `tools/perf/bench-thresholds.json`: the desktop
budgets plus 10 % for the timing server, gated on the median of five runs (RelWithDebInfo, Linux
x64, 8 workers). The fixtures:

- **bench_nav_city**: a 256 x 256 m terrain of gentle hills (6 m) with 200 box buildings,
  cell 0.2 m, 64-cell tiles: 400 tiles, about 2.6k polygons (the spec's "about 20k" assumed a
  busier city; the tile and cell sizes are the spec's). Roofs are walkable islands: random pairs
  that land on one are timed apart (`query_path_unreachable_p95_ms`, they search the whole
  2048-node budget and fail).
- **crowds**: 100 and 500 agents (Good avoidance) walking to random destinations across the city,
  the navigation system's whole fixed tick (sync, crowd, write back, events).
- **carving**: a carving box moved across a corridor of a Runtime Carving surface.
- **grids**: a 256^2 serpentine maze (walls every 4th column, open at alternating ends: every
  reachable cell is expanded, the literal worst case) from corner to corner; an unreachable goal
  behind a closed wall; 200 random requests on a 128^2 grid with 15 % blocked cells, partial
  paths allowed, on 4 threads.
- **empty**: a scene without navigation components.

## Results

Server E, median of 5 runs (one RelWithDebInfo build, five `je-bench-nav --count-allocs` runs and
the median of each metric):

| Metric | Budget (spec) | E, median of 5 | |
|---|---|---|---|
| `city_bake_full_8core_projected_s` | <= 10 s (R28) | 0.12 s | ok |
| `city_bake_full_1t_s` | <= 40 s (R28) | 0.56 s | ok |
| `city_bake_incremental_ms` / `_tiles` | <= 400 ms, <= 4 tiles (R28) | 4.9 ms, 4 tiles | ok |
| `city_navmesh_ram_mb` / `_disk_mb` | <= 8 MB / <= 4 MB (R30) | 0.62 MB / 0.51 MB | ok |
| `query_path_p95_ms` | <= 0.15 ms (R26) | 0.129 ms | ok |
| `query_sample_p95_ms` / `query_raycast_p95_ms` | <= 0.02 ms (R26) | 0.002 ms / 0.002 ms | ok |
| `crowd_100_tick_ms` (p95) | <= 0.6 ms (R27) | 0.30 ms (0.45) | ok |
| `crowd_500_tick_ms` (p95) | <= 3 ms (R27) | 2.18 ms (2.48) | ok |
| `carve_tick_max_ms` / `carve_ticks_to_land` | <= 1 ms / <= 2 ticks (R28) | 0.19 ms / 1 tick | ok |
| `grid_256_worst_ms` | <= 2 ms (R29) | 4.32 ms | **missed** |
| `grid_reject_us` | <= 5 us (R29) | 0.04 us | ok |
| `grid_batch_200_4t_ms` | <= 4 ms (R29) | 10.9 ms | **missed** |
| `grid_bytes_per_cell` | 1 B (+4 B index, R29) | 5.1 B | ok |
| `empty_tick_us` / allocations | < 1 us / 0 (R25) | 0.005 us / 0 | ok |

Informational: collect 2.1 ms, 400 tiles, 2643 polys, unreachable path p95 0.33 ms (the whole
2048-node budget searched).

The two grid budgets are missed: the serpentine maze expands every one of its ~49k reachable cells
(about 88 ns per expanded cell, heap included), and the batch's random 15 %-blocked grid costs about
0.2 ms per path. Both run the general A* (area costs, reservations, slope limits, no corner
cutting); a uniform-cost accelerator (jump points) is the follow-up that would bring them in.

## Notes

- The web and mobile budgets of R26-R29 are recorded by the S15 profiler run on the device lab
  (release gate); the single-threaded web bake's main-thread slices are checked by the web gate
  (`tools/web/gates/navigation.json`: no slice over the web bake budget + 2 ms).
- The wasm size delta (R30, <= 450 KB) is the S02 size check's.
