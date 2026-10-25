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
| B1 blank | 2 -> 1 | 0 -> 0 | 0 -> 0 | 1 -> 0 | 0.017 -> 0.014 | 0.004 / 0.005 / 0.002 (baseline replay 0.002) | 5.2 -> 8.1 |
| B1 dialogue | 43 -> 27 | 2 -> 2 | 6 -> 5 | 3 -> 0 | 0.212 -> 0.221 | 0.021 / 0.063 / 0.116 (baseline replay 0.114) | 105.7 -> 111.2 |
| B1 hud | 155 -> 121 | 28 -> 18 | 16 -> 16 | 11 -> 0 | 0.466 -> 0.528 | 0.036 / 0.145 / 0.316 (baseline replay 0.250) | 146.7 -> 165.4 |
| B1 inventory | 46 -> 28 | 2 -> 2 | 4 -> 4 | 1 -> 0 | 0.209 -> 0.241 | 0.028 / 0.095 / 0.095 (baseline replay 0.063) | 71.2 -> 163.3 |
| B1 loading | 57 -> 53 | 4 -> 2 | 5 -> 5 | 3 -> 0 | 0.583 -> 0.618 | 0.096 / 0.350 / 0.156 (baseline replay 0.134) | 146.7 -> 172.7 |
| B1 main_menu | 105 -> 76 | 18 -> 0 | 17 -> 17 | 0 -> 0 | 0.504 -> 0.565 | 0.092 / 0.167 / 0.278 (baseline replay 0.213) | 233.0 -> 264.5 |
| B1 pause_menu | 42 -> 24 | 0 -> 0 | 5 -> 5 | 3 -> 0 | 0.184 -> 0.205 | 0.029 / 0.085 / 0.077 (baseline replay 0.068) | 70.0 -> 114.9 |
| B1 settings | 97 -> 45 | 38 -> 8 | 4 -> 4 | 5 -> 0 | 0.358 -> 0.391 | 0.072 / 0.184 / 0.115 (baseline replay 0.097) | 74.4 -> 161.1 |
| B1 shop | 26 -> 17 | 0 -> 0 | 2 -> 2 | 2 -> 0 | 0.129 -> 0.153 | 0.030 / 0.049 / 0.055 (baseline replay 0.037) | 42.4 -> 78.4 |
| B1 toasts | 82 -> 61 | 30 -> 12 | 9 -> 9 | 2 -> 0 | 0.316 -> 0.336 | 0.076 / 0.077 / 0.164 (baseline replay 0.148) | 97.5 -> 110.1 |
| B1 video_menu | 32 -> 26 | 0 -> 0 | 1 -> 1 | 0 -> 0 | 0.194 -> 0.211 | 0.065 / 0.099 / 0.045 (baseline replay -0.008) | 59.8 -> 121.9 |
| B2 list-500 | 1115 -> 4 | 10 -> 0 | 2 -> 0 | 2 -> 0 | 3.404 -> 3.235 | 0.564 / 2.500 / 0.117 (baseline replay 0.404) | 63.8 -> 198.5 |
| B3 text-wall | 39 -> 1 | 0 -> 0 | 0 -> 0 | 0 -> 0 | 0.078 -> 0.095 | 0.010 / 0.049 / 0.018 (baseline replay 0.019) | 36.7 -> 128.5 |
| B5 text-effects | 187 -> 100 | 0 -> 0 | 17 -> 0 | 0 -> 0 | 0.390 -> 0.252 | 0.029 / 0.121 / 0.077 (baseline replay 0.216) | 77.6 -> 114.0 |
| B6 list-500, 1024² texture | 1097 -> 4 | 10 -> 0 | 2 -> 0 | 2 -> 0 | 3.582 -> 3.264 | 0.579 / 2.540 / 0.111 (baseline replay 0.409) | 62.1 -> 178.5 |

- B4 scale-popin: the 40 labels settle 28 frames after the start with exactly 40 regenerations
  (one per label), no frame over 2 ms of glyph rasterization or 256 glyphs.
- Gathering (R22): 9,708 vertices (an instance counted as the 4 vertices it replaces) in 0.106 ms,
  0.110 ms per 10k (target 0.2).
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

llvmpipe pays per instance: the same 5,000 glyphs take 128 ms a frame as one instanced draw and
38 ms as one vertex run (37 ms baseline). The frame times above are that cost, not a GPU's; R24's
GPU pass timings need a GPU runner.
