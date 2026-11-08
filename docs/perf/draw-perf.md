# Draw performance

What the draw-perf changes ([spec](../superpowers/specs/2026-11-04-draw-perf-design.md)) save and
how it is measured: the pipelines and bindings a frame applies again, the opaque queue's order,
the moved-object math, and the per-frame work of features that are off.

## How it is measured

- `DrawPerfGpu` (core tests, glcore and Vulkan; GPU Instancing off so each draw is one draw)
  prints `[draw-perf]` lines: the draws, pipelines and bindings applied of fixed scenes, and what
  the visible list culled. Its assertions are the red controls (each fails without the change).
- `DrawPerfBench` (core tests) prints `[bench]` lines for the moved-object math against the
  general forms it replaces; with `JASPER_TEST_PERF_SLACK` set (a quiet server) it enforces the
  budgets: closed-form `T * R * S` at least 2x faster, the affine AABB transform at least 2x
  faster than the corner loop, the 3x3 determinant at least 4x faster than the 4x4 one.

```sh
UNIT_SUITE=core UNIT_TEST_ARGS='--gtest_filter=DrawPerfGpu.*:DrawPerfBench.*' tools/build.sh unit-tests
GRAPHIC_BACKEND=vulkan UNIT_SUITE=core UNIT_TEST_ARGS='--gtest_filter=DrawPerfGpu.*' tools/build.sh unit-tests
```

## Results

Measured 2026-11-06 on build box A (Hetzner cpx51, 16 vCPU, no GPU: Mesa llvmpipe for glcore,
lavapipe for Vulkan), in the linux build image.

| Scene (GPU Instancing off) | Before (`main`) | After | Backends |
|---|---|---|---|
| 30 Models of one file, one material: pipelines / bindings applied for 30 draws | 30 / 30 | 1 / 1 | glcore, Vulkan |
| 20 boxes in one band, alternately lit and unlit: pipelines applied (before: the frame; after: its colour pass) | 20 | 2 | glcore, Vulkan |
| 100 boxes, 90 behind the camera: culled before the sort | 0 | 90 | glcore, Vulkan |

The "before" column is the red-control run of `DrawPerfGpu` with the changes reverted (local
build slot, glcore). On D3D11 every applied pipeline sets the rasterizer, blend, depth-stencil,
input layout, shaders and constant buffers again, and on Vulkan every bindings apply writes a
descriptor set into the frame's descriptor buffer: those are the calls these counts no longer make.

| Moved-object math, 10,000 each (median of 15) | General | Affine | Speed-up | Budget |
|---|---|---|---|---|
| local matrix: `T * R * S` vs `composeTRS` | 898 us | 236 us | 3.8x | 2x |
| AABB transform: 8 corners vs per-axis intervals | 2,428 us | 535 us | 4.5x | 2x |
| mirrored-draw test: 4x4 vs 3x3 determinant | 154 us | 28 us | 5.5x | 4x |

The directional-only Lit (`Dlo`) is checked for the same image (`DrawPerfGpu.DirectionalOnlyDrawsTheSameImage`:
no channel more than 1 apart); its GPU time was not measured (llvmpipe and lavapipe are not the
mobile GPUs it is for).
