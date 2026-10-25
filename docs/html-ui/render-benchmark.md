# HTML UI rendering benchmark (S16)

`HtmlUIRenderPerf.*` (`engine/tests/htmlui/RenderPerfTest.cpp`, Release builds of
`jasperengine-htmlui-tests`), documents in `engine/tests/htmlui/fixtures/bench/` and the example
library (`docs/mcp/guide/html-ui-examples`). Each benchmark draws its document with every S16 layer
off (the baseline: no batching, no SDF primitives, no transform-aware text) and with the defaults,
in the same process; the results go to `build/linux/htmlui-render-bench.json`.

    HTMLUI_TEST_ARGS=--gtest_filter=HtmlUIRenderPerf.* tools/build.sh htmlui-tests

Host: the benchmark-only server (4 dedicated vCPU, GL core on llvmpipe, Release), idle, run of
2026-10-25. "Engine" is the median wall time of 120 frames after 30 warm-ups less the time spent in
the graphics backend (`RenderBackendTime`): what S16 changes on the CPU. "Frame" is the median wall
time with the backend: on llvmpipe it is the software rasterizer's cost and no GPU measure (below).

## Draws and CPU

| Benchmark | Draws | Stencil draws | Layers | Box-shadow textures | Engine ms | Update / record / replay ms (S16) | Frame ms (llvmpipe) |
|---|---|---|---|---|---|---|---|
| B1 blank | 2 -> 1 | 0 -> 0 | 0 -> 0 | 1 -> 0 | 0.013 -> 0.015 | 0.004 / 0.005 / 0.002 | 4.9 -> 6.9 |
| B1 dialogue | 43 -> 27 | 2 -> 2 | 6 -> 5 | 3 -> 0 | 0.202 -> 0.209 | 0.022 / 0.063 / 0.102 | 101.9 -> 99.4 |
| B1 hud | 155 -> 121 | 28 -> 18 | 16 -> 16 | 11 -> 0 | 0.497 -> 0.497 | 0.036 / 0.155 / 0.276 | 141.3 -> 156.7 |
| B1 inventory | 46 -> 28 | 2 -> 2 | 4 -> 4 | 1 -> 0 | 0.203 -> 0.228 | 0.028 / 0.104 / 0.079 | 66.0 -> 138.2 |
| B1 loading | 57 -> 53 | 4 -> 2 | 5 -> 5 | 3 -> 0 | 0.571 -> 0.572 | 0.091 / 0.316 / 0.140 | 144.8 -> 162.1 |
| B1 main_menu | 105 -> 76 | 18 -> 0 | 17 -> 17 | 0 -> 0 | 0.500 -> 0.517 | 0.089 / 0.164 / 0.237 | 223.7 -> 253.8 |
| B1 pause_menu | 42 -> 24 | 0 -> 0 | 5 -> 5 | 3 -> 0 | 0.175 -> 0.213 | 0.029 / 0.080 / 0.085 | 67.0 -> 102.1 |
| B1 settings | 97 -> 45 | 38 -> 8 | 4 -> 4 | 5 -> 0 | 0.327 -> 0.353 | 0.060 / 0.166 / 0.103 | 69.6 -> 136.7 |
| B1 shop | 26 -> 17 | 0 -> 0 | 2 -> 2 | 2 -> 0 | 0.120 -> 0.156 | 0.032 / 0.056 / 0.049 | 40.9 -> 68.5 |
| B1 toasts | 82 -> 61 | 30 -> 12 | 9 -> 9 | 2 -> 0 | 0.299 -> 0.316 | 0.075 / 0.074 / 0.143 | 95.6 -> 108.3 |
| B1 video_menu | 32 -> 26 | 0 -> 0 | 1 -> 1 | 0 -> 0 | 0.176 -> 0.205 | 0.066 / 0.093 / 0.043 | 54.3 -> 105.3 |
| B2 list-500 | 1115 -> 5 | 10 -> 0 | 2 -> 0 | 2 -> 0 | 3.385 -> 3.134 | 0.550 / 2.439 / 0.108 (baseline replay 0.385) | 53.9 -> 159.4 |
| B3 text-wall | 39 -> 2 | 0 -> 0 | 0 -> 0 | 0 -> 0 | 0.066 -> 0.093 | 0.009 / 0.048 / 0.016 | 29.6 -> 104.2 (as vertex runs: 30.9) |
| B5 text-effects | 187 -> 100 | 0 -> 0 | 17 -> 0 | 0 -> 0 | 0.386 -> 0.252 | 0.031 / 0.122 / 0.077 | 71.9 -> 104.3 |
| B6 list-500, 1024² texture | 1097 -> 5 | 10 -> 0 | 2 -> 0 | 2 -> 0 | 3.439 -> 3.254 | | 54.0 -> 143.0 |

- B4 scale-popin: the 40 labels settle 28 frames after the start with exactly 40 regenerations
  (one per label), no frame over 2 ms of glyph rasterization or 256 glyphs.
- Gathering (R22): 9,708 vertices (an instance counted as the 4 vertices it replaces) in 0.108 ms,
  0.111 ms per 10k (target 0.2).
- B3 atlas (R23): one 1 MiB R8 page where RGBA8 took 4 MiB.

## What the B1 draws are

Why each run of the batched replay started (`RmlRenderer::getLastRunBreaks`, S16 defaults): after
a command that ends runs (layer, composite, filter, gradient or other shader, stencil mask), a change
of kind (vertices / instances), clip or layer, transform, texture, or scissor.

| Example | Runs: command / kind / clip / transform / texture / scissor | Draws that are not runs |
|---|---|---|
| dialogue | 5 / 2 / 0 / 0 / 1 / 0 | 19 |
| hud | 21 / 0 / 2 / 0 / 9 / 0 | 89 |
| inventory | 6 / 0 / 0 / 0 / 11 / 0 | 11 |
| loading | 8 / 0 / 2 / 0 / 3 / 0 | 40 |
| main_menu | 16 / 0 / 0 / 4 / 8 / 0 | 48 |
| pause_menu | 5 / 0 / 0 / 0 / 10 / 0 | 9 |
| settings | 10 / 0 / 4 / 2 / 12 / 0 | 17 |
| shop | 3 / 0 / 0 / 0 / 9 / 0 | 5 |
| toasts | 12 / 0 / 3 / 0 / 3 / 0 | 43 |
| video_menu | 5 / 0 / 0 / 0 / 10 / 0 | 11 |

The draws that are not runs are the examples' effects: filter and backdrop-blur passes, gradients,
layer composites and stencil masks, which S16 leaves as they are. Of the runs, most are split by
textures (icons, images, the glyph pages, effect variants): one texture per instanced draw (D3).
R21's "<= 30 % of the baseline and <= 40 draws" is therefore not met by the examples (spec
Deviations); a few texture slots per `HTMLUI_PRIM` draw would merge the texture splits.

## Software rasterizer

llvmpipe pays per instance: the same 5,000 glyphs take 104 ms a frame as 2 instanced draws and
31 ms as 3 vertex runs (30 ms baseline). The frame times above are that cost, not a GPU's; R24's
GPU pass timings need a GPU runner.
