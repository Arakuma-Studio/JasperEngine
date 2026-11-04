# Foliage: budgets and measured numbers

T5 R40-R45, gated by `je-bench-foliage` through `tools/perf/check_bench.py` and
`tools/perf/bench-thresholds.json` on the reference Linux x64 Release build. Software GL says nothing about
GPU cost: no figure here is a device frame time. Real-device numbers are `docs/specs/checklists/T5.md`'s.

The benchmark scene is `FoliageBenchScene`, made by code (`engine/tests/core/bench/FoliageBench.cpp`): a
1024-unit Terrain, 1024^2 16-bit hash-fractal height, 512^2 density maps, 4 detail layers at 4, 2, 1 and 0.5
per square unit, 3 tree prototypes with 50,000 trees, 600 frames on a fixed path at 5 units per second,
turning 30 degrees per second, Detail Distance 120, Tree Distance 1000, Billboard Start 80.

## Measured

See the table `tools/perf/bench-thresholds.json` gates; the numbers below are from the run named with them.

MEASURED_TABLE

## Size

The web player's growth by T5, by S34's method (`tools/build.sh web`, gzipped wasm, before and after):

SIZE_TABLE
