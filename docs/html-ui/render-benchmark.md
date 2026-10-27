# HTML UI rendering benchmark (S16)

`HtmlUIRenderPerf.*` (`engine/tests/htmlui/RenderPerfTest.cpp`, Release builds of
`jasperengine-htmlui-tests`), documents in `engine/tests/htmlui/fixtures/bench/` and the example
library (`docs/mcp/guide/html-ui-examples`). Each benchmark draws its document with every S16 layer
off (the baseline: no batching, no SDF primitives, no transform-aware text, RmlUi's paint traversal
without the S16-F4/F6 shortcuts) and with the defaults, in the same process; the results go to
`build/linux/htmlui-render-bench.json`.

    HTMLUI_TEST_ARGS=--gtest_filter=HtmlUIRenderPerf.* tools/build.sh htmlui-tests

Host: the benchmark-only server (4 dedicated vCPU, GL core on llvmpipe, Release), idle, run of
2026-10-26 (round 2). "Engine" is the median wall time of 120 frames after 30 warm-ups less the
time spent in the graphics backend (`RenderBackendTime`), each frame started with the GPU idle
(`finishGpu`: llvmpipe's threads, one per core, would otherwise take the cores from the engine's
thread while they draw the last frame, more for the heavier frame). "Frame" is the median wall time
with the backend: on llvmpipe it is the software rasterizer's cost and no GPU measure (below).

## Draws and CPU

| Benchmark | Draws | Stencil draws | Layers | Box-shadow textures | Engine ms (S16 / baseline) | Update / record / replay ms | Frame ms (llvmpipe) |
|---|---|---|---|---|---|---|---|
| B1 blank | 2 -> 1 | 0 -> 0 | 0 -> 0 | 1 -> 0 | 0.029 -> 0.039 (135 %) | 0.011 / 0.012 / 0.006 (baseline 0.009 / 0.009 / 0.003) | 2.2 -> 5.0 |
| B1 dialogue | 43 -> 14 | 2 -> 2 | 6 -> 1 | 3 -> 0 | 0.212 -> 0.154 (73 %) | 0.024 / 0.052 / 0.057 (baseline 0.023 / 0.057 / 0.112) | 97.7 -> 94.0 |
| B1 hud | 155 -> 80 | 28 -> 14 | 16 -> 6 | 11 -> 0 | 0.451 -> 0.338 (75 %) | 0.037 / 0.115 / 0.167 (baseline 0.038 / 0.127 / 0.268) | 133.3 -> 87.8 |
| B1 inventory | 46 -> 6 | 2 -> 0 | 4 -> 0 | 1 -> 0 | 0.199 -> 0.167 (84 %) | 0.031 / 0.092 / 0.019 (baseline 0.030 / 0.089 / 0.059) | 65.4 -> 153.4 |
| B1 loading | 57 -> 34 | 4 -> 2 | 5 -> 3 | 3 -> 0 | 0.540 -> 0.523 (97 %) | 0.096 / 0.290 / 0.114 (baseline 0.092 / 0.308 / 0.120) | 137.4 -> 488.9 |
| B1 main_menu | 105 -> 36 | 18 -> 0 | 17 -> 3 | 0 -> 0 | 0.467 -> 0.319 (68 %) | 0.082 / 0.123 / 0.096 (baseline 0.083 / 0.131 / 0.224) | 216.0 -> 128.7 |
| B1 pause_menu | 42 -> 2 | 0 -> 0 | 5 -> 1 | 3 -> 0 | 0.187 -> 0.133 (71 %) | 0.029 / 0.069 / 0.011 (baseline 0.029 / 0.070 / 0.068) | 64.2 -> 121.7 |
| B1 settings | 97 -> 17 | 38 -> 4 | 4 -> 0 | 5 -> 0 | 0.308 -> 0.251 (82 %) | 0.066 / 0.126 / 0.034 (baseline 0.064 / 0.141 / 0.079) | 69.3 -> 142.7 |
| B1 shop | 26 -> 2 | 0 -> 0 | 2 -> 0 | 2 -> 0 | 0.132 -> 0.111 (84 %) | 0.031 / 0.048 / 0.010 (baseline 0.030 / 0.046 / 0.037) | 38.2 -> 97.9 |
| B1 toasts | 82 -> 40 | 30 -> 12 | 9 -> 3 | 2 -> 0 | 0.285 -> 0.229 (80 %) | 0.070 / 0.063 / 0.080 (baseline 0.072 / 0.061 / 0.130) | 90.3 -> 38.7 |
| B1 video_menu | 32 -> 14 | 0 -> 0 | 1 -> 1 | 0 -> 0 | 0.189 -> 0.212 (112 %) | 0.071 / 0.089 / 0.043 (baseline 0.063 / 0.084 / 0.029) | 54.6 -> 200.3 |
| B2 list-500 | 1115 -> 3 | 10 -> 0 | 2 -> 0 | 2 -> 0 | 2.477 -> 1.084 (44 %) | 0.402 / 0.617 / 0.039 (baseline 0.400 / 1.654 / 0.399) | 60.0 -> 216.2 |
| B3 text-wall | 39 -> 1 | 0 -> 0 | 0 -> 0 | 0 -> 0 | 0.088 -> 0.101 (115 %) | 0.011 / 0.054 / 0.013 (baseline 0.009 / 0.044 / 0.019) | 33.3 -> 138.6 |
| B5 text-effects | 187 -> 1 | 0 -> 0 | 17 -> 0 | 0 -> 0 | 0.372 -> 0.182 (49 %) | 0.027 / 0.112 / 0.019 (baseline 0.028 / 0.116 / 0.211) | 70.8 -> 126.4 |
| B6 list-500, 1024² texture | 1097 -> 3 | 10 -> 0 | 2 -> 0 | 2 -> 0 | 2.485 -> 1.073 (43 %) | 0.400 / 0.606 / 0.040 (baseline 0.398 / 1.670 / 0.396) | 57.2 -> 192.9 |
| B7 virtual-list scroll | 122 -> 3 | 4 -> 0 | 0 -> 0 | 0 -> 0 | 1.174 -> 1.003 (85 %) | 0.581 / 0.345 / 0.045 (baseline 0.573 / 0.503 / 0.076) | 37.6 -> 128.1 |

- B2 (R22, <= 50 %): 44 % (B6: 43 %); on the tree merged with oe-port e1ca0c71a: 2.364 -> 1.039 ms (44 %), B6 45 %; on the replacement benchmark host with oe-port 05eb8d3b4 merged: 2.547 -> 1.144 ms (45 %), B6 46 %; with 545618e38 merged (11ee1a967): 2.652 -> 1.127 ms (42 %), B6 43 %; with 697d661e2 merged (0ef7ff94f): 2.591 -> 1.123 ms (43 %), B6 2.538 -> 1.103 ms (43 %); the nine benchmarks pass on each. The replay is batched (0.40 -> 0.04 ms) and the paint
  traversal passes over the 450 cards outside the list's scissor (S16-F4, S16-F6: 1.65 -> 0.62 ms).
- B7 is S17's `<list-view>` of 10,000 rows (a C++ provider) scrolled 100 px a frame: only the
  visible rows exist, so there is nothing to cull; 122 -> 3 draws, the engine time at 85 %.
- B4 scale-popin: the 40 labels settle 28 frames after the start with exactly 40 regenerations
  (one per label), no frame over 2 ms of glyph rasterization or 256 glyphs.
- Gathering (R22): 4,248 vertices (an instance counted as the 4 vertices it replaces) in 0.053 ms,
  0.124 ms per 10k (target 0.2).
- B3 atlas (R23): one 1 MiB R8 page where RGBA8 took 4 MiB.

## What the B1 draws are

Why each run of the batched replay started (`RmlRenderer::getLastRunBreaks`, S16 defaults): after
a command that ends runs (layer, composite, filter, effect shader, stencil mask), a change of kind
(vertices / instances), clip or layer, transform, texture, or scissor; and the frame's effect draws
(`RmlRenderer::getLastEffectCounts`).

| Example | Draws (R21 target) | Runs: command / kind / clip / transform / texture / scissor | Effect shaders / composites / filters / clip masks | Stencil draws |
|---|---|---|---|---|
| dialogue | 14 (target 12) | 2 / 2 / 0 / 0 / 0 / 0 | 0 / 2 / 1 / 1 | 2 |
| hud | 80 (target 40) | 10 / 0 / 7 / 0 / 0 / 0 | 0 / 11 / 6 / 14 | 14 |
| inventory | 6 (target 13) | 2 / 0 / 2 / 0 / 1 / 0 | 1 / 0 / 0 / 1 | 0 |
| loading | 34 (target 17) | 6 / 0 / 2 / 0 / 0 / 0 | 0 / 4 / 3 / 2 | 2 |
| main_menu | 36 (target 31) | 6 / 0 / 0 / 9 / 1 / 0 | 4 / 4 / 3 / 9 | 0 |
| pause_menu | 2 (target 12) | 1 / 0 / 0 / 0 / 1 / 0 | 0 / 2 / 2 / 0 | 0 |
| settings | 17 (target 29) | 4 / 0 / 4 / 2 / 1 / 1 | 1 / 0 / 0 / 19 | 4 |
| shop | 2 (target 7) | 1 / 0 / 0 / 0 / 1 / 0 | 0 / 0 / 0 / 0 | 0 |
| toasts | 40 (target 24) | 6 / 0 / 3 / 0 / 0 / 0 | 0 / 6 / 3 / 15 | 12 |
| video_menu | 14 (target 9) | 4 / 0 / 0 / 0 / 1 / 0 | 1 / 1 / 1 / 0 | 0 |


Round 2 removed the texture splits (four texture slots per `HTMLUI_PRIM` draw) and the gradients'
shader draws and mask layers (CSS background gradients as primitives, S16-F5): R21 is met by
blank, inventory, pause_menu, settings and shop. The rest of the draws are the examples' effects:
backdrop-filter and filter blurs (each several passes), `text-shadow` blurs beyond the distance
glyphs' reach (main_menu's 24 px glow: a layer and a blur), opacity groups and their composites,
and stencil masks under layers (an analytic clip cannot clip a composite). They are drawn as the
browser draws them; the spec's Deviations keep R21 reported, not gated, for those six.

## Software rasterizer

llvmpipe pays per instance and per fragment of the SDF shader: the same 5,000 glyphs take 138 ms a
frame as one instanced draw and 34 ms as one vertex run (33 ms baseline). The frame times above
are that cost, not a GPU's; R24's GPU pass timings need a GPU runner.
