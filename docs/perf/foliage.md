# Foliage: budgets and measured numbers

T5 R40-R45, gated by `je-bench-foliage` through `tools/perf/check_bench.py` and
`tools/perf/bench-thresholds.json` on the reference Linux x64 Release build. Software GL says nothing about
GPU cost: no figure here is a device frame time. Real-device numbers are `docs/specs/checklists/T5.md`'s.

The benchmark scene is `FoliageBenchScene`, made by code (`engine/tests/core/bench/FoliageBenchScene.h`): a
1024-unit Terrain, 1024^2 16-bit hash-fractal height, 512^2 density maps, 4 detail layers at 4, 2, 1 and 0.5
per square unit, 3 tree prototypes with 50,000 trees, 600 frames on a fixed path at 5 units per second,
turning 30 degrees per second, Detail Distance 120, Tree Distance 1000, Billboard Start 80.

## Measured

See the table `tools/perf/bench-thresholds.json` gates; the numbers below are from the run named with them.

One run of `je-bench-foliage` on the 8-core build host **with other builds running**, so these are not the gate's
numbers: the budgets are to be enforced on an idle host (R41), and that run is still to do. Times marked over
budget here have not been shown to be over it on an idle host.

| Metric | Budget | This run |
|---|---|---|
| One placement attempt | 250 ns | 209 ns |
| A full-density cell (1,280 attempts) | 0.35 ms | 0.067 ms |
| Cull of 100,000 instances, 0 workers | 3.0 ms | 2.06 ms |
| The same, 4 workers | 1.2 ms | 2.36 ms (over, under load) |
| Foliage main thread, moving camera, p99 | 1.5 ms | 9.27 ms (p50 2.76 ms; over, under load) |
| A still frame | 50 us | 18 us |
| An empty scene | 1 us, no allocation | 0.011 us, 0 allocations |
| A stamp of 8 trees | 2 ms | 0.066 ms |
| Cells rebuilt by 100 trees in one cell | 9 | 1 |
| A stored tree | 40 bytes | 40 bytes |
| Memory, CPU and GPU, without meshes and impostors | 128 MB | 14 MB |

`FoliageBudget.BenchSceneWithinCounts*` walks the same path through whole frames. Most visible at once: High 60,297
details and 12,295 trees; Low 29,995 details and 8,000 trees, each under R40's caps.

The before-picture of R44 (the same detail-only scene on the commit before T5) was not measured: it needs a
second engine tree of that commit.

## Size

The web player's growth by T5, by S34's method (`tools/build.sh web`, gzipped wasm, before and after), is
measured on the merged tree with the programme's web build: not yet run. R43's limit is 64 KiB.
