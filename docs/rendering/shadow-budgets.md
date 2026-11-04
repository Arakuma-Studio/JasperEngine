# Shadow budgets

The cost budgets of the 3D shadows (spec S10 §5, AC14 to AC16) and what has been measured
against them. The tiers' settings are `engine/core/render/shadow/ShadowTiers.cpp`; the user
reference is `docs/rendering/shadows.md`.

## The budgets

| Tier (default for) | Low (Android, iOS) | Medium (Web) | High (Win/Linux/macOS) | Ultra (opt-in) |
|---|---|---|---|---|
| Max cascades / slot res | 2 / 1024 | 3 / 2048 | 4 / 2048 | 4 / 4096 |
| Default shadow distance | 40 m | 60 m | 100 m | 150 m |
| Soft filter | 3x3 grid | 5x5 grid | Vogel 16 | PCSS 16+16..32 |
| Receiver GPU, full-screen sun, Soft | ≤ 1.0 ms | ≤ 1.5 ms | ≤ 0.6 ms | ≤ 1.2 ms |
| Shadow pass GPU, static scene and camera, cached | ≤ 0.05 ms | ≤ 0.05 ms | ≤ 0.05 ms | ≤ 0.05 ms |
| Shadow pass GPU, walking camera (5 m/s), cached + 20 dynamic | ≤ 1.5 ms | ≤ 2.0 ms | ≤ 1.0 ms | ≤ 1.5 ms |
| Shadow memory (atlases + static cache) | ≤ 16 MB | ≤ 48 MB | ≤ 192 MB | ≤ 576 MB |

CPU (R26): the cascade fit and the cache keys of a camera ≤ 30 µs; the caster registry's update
of 10,000 casters with 1 % moving ≤ 0.2 ms; a frame with nothing changed issues no shadow draw
call; a scene without shadows costs nothing.

Reference devices: mobile Adreno 640 / Mali-G76 class at 1080p (GLES3 and Vulkan), web Intel Iris
Xe in Chrome (WebGL2) at 1080p, desktop GTX 1660 class at 1440p.

## How to measure

The bench scene is the `tools/fixtures/shadows` project: `scripts/Shadows.lua` builds 1 sun, 4 spot
lights, 2 point lights, 2,000 static casters (`staticShadowCaster`) and 20 animated skinned and
morphing dynamic casters at runtime. Its environment variables pick the case:

| Variable | Effect |
|---|---|
| `SHADOWS_QUALITY=low\|medium\|high\|ultra` | forces a tier (else the platform's) |
| `SHADOWS_WALK=1` | the camera walks at 5 m/s (the walking row); else it holds still |
| `SHADOWS_STATIC=<n>`, `SHADOWS_DYNAMIC=<n>` | caster counts (2000, 20); `SHADOWS_DYNAMIC=0` with a still camera is the "static scene and camera" row |
| `JASPERENGINE_FRAME_STATS=1` (with `JASPERENGINE_READY_SIGNAL=1`) | logs the passes and draws of frames 2, 30 and 90 after the scene is ready |

Export it for the device (`jasperengine-editor export`, or the editor's Build window) and run it
with those variables; on Linux `tools/smoke-test.sh tools/fixtures/shadows` does both, with
`SMOKE_GAME_ENV` and `SMOKE_GAME_RUNS` for the variables. GPU pass times come from the GPU pass
timers of the profiling stream (S15) once it lands: `get_shadow_stats` reports `gpu_ms` from them
(null until then), and the frame stats list the shadow passes by name (`shadow`, `point shadow`,
`shadow static`, `point shadow static`). Memory is `get_shadow_stats.memory_bytes`.

## Measured

Build server D (16 vCPU, shared with other builds; Docker, Mesa llvmpipe for OpenGL), October
2026. GPU times on the reference devices have not been measured yet: the build servers have no
GPU, and the GPU pass timers (S15) are not on `oe-port`. Until they are, the tiers' GPU budget rows
are unverified, and the numbers below are what a software renderer can check.

**Zero cost (AC15).** `tools/fixtures/shadows-off` (a lit scene: a sun without shadows, a ground and
five boxes) through `tools/smoke-test.sh` with `JASPERENGINE_FRAME_STATS=1`, on `oe-port` before
the shadows (cf75a9d69) and on this branch:

| Build | Passes | Draws | Pipelines applied | Uniform uploads | Images alive | Views alive |
|---|---|---|---|---|---|---|
| `oe-port` | 1 (camera) | 6 | 6 | 24 (5,088 B) | 10 | 10 |
| S10 | 1 (camera) | 6 | 6 | 24 (5,088 B) | 10 | 11 |

The one more view is the 1×1 raw-depth stand-in (`emptyShadowDepthRaw`), made once at startup.

**CPU (R26)**, `jasperengine-core-tests` (`ShadowBench`, RelWithDebInfo):

| | Measured | Budget |
|---|---|---|
| Cascade fit (4 cascades) + 9 slot keys, per camera | 1.7 µs median | ≤ 30 µs |
| Caster registry, 10,000 casters, 1 % moving | 0.13 ms median | ≤ 0.2 ms |

**The bench scene's passes** (`tools/smoke-test.sh tools/fixtures/shadows`, OpenGL on llvmpipe,
`JASPERENGINE_FRAME_STATS=1`, `SMOKE_READY_SETTLE=60`; the camera pass alone draws 948 times):

| Case | Shadow passes | Draws in the frame |
|---|---|---|
| Static scene and camera, cached (`SHADOWS_DYNAMIC=0`) | none | 948 (the camera pass only) |
| Still camera, 20 dynamic casters (High) | `shadow`, `point shadow` | 5,122 |
| Walking camera, 20 dynamic casters (High) | `shadow`, `point shadow` | 4,549 |
| Still camera, 20 dynamic casters, Low | `shadow`, `point shadow` | 3,323 |
| Still camera, 20 dynamic casters, Ultra | `shadow`, `point shadow` | 5,493 |

With every caster still, a frame issues no shadow draw at all (R26, AC15). With the 20 animated
casters the slots they cross are redrawn every frame in full: the bench's atlases alone nearly
fill each tier's memory row (High: four 2048² cascades, four 1024² spot maps and twelve 512² cube
faces in D32 are 92 MB of the 96 MB), so the planner, which fits the atlases first and the static
layers in what remains, gives this scene none. The static layer pays in scenes whose atlases leave
room for it (the MCP cache tests' scenes); the memory rows and the default resolutions are the
numbers to revisit once the reference-device measurements exist.

**Memory (AC16)**: `ShadowSlotPlanner.MemoryBudgetDropsStaticLayersThenHalves` plans the bench
scene's lights at every tier within the tier's row; `test_shadows.py::test_shadow_goldens` checks
`memory_bytes ≤ memory_budget_bytes` at every tier through `get_shadow_stats`.

## Measured on a GPU (AC14, the GPU campaign)

NVIDIA GeForce RTX 3080 Ti, driver 595.99.02, OpenGL 4.6 (GLX on a headless Xorg) and Vulkan,
October 2026 (`docs/perf/gpu-campaign.md`): `tools/perf/gpu_campaign.py shadows --runs 5` plays the
bench fixture in the editor with the Game view sized to the tier's resolution (desktop 2560x1440,
web and mobile 1920x1080) and reads the S15 GPU pass timers over 300 frames; medians of 5 runs.
The shadow passes are `shadow`, `point shadow`, `shadow static` and `point shadow static`; the
receiver row is the camera pass of the ground alone under the sun, soft against no shadow
(`SHADOWS_STATIC=0 SHADOWS_LIGHTS=sun SHADOWS_SUN=soft|none`).

| OpenGL, RTX 3080 Ti | Receiver, sun soft | Budget | Static scene and camera, cached | Budget | Walking, cached + 20 dynamic | Budget |
|---|---|---|---|---|---|---|
| Low, 1080p | 0.061 ms | ≤ 1.0 | 0.000 | ≤ 0.05 | 1.209 | ≤ 1.5 |
| Medium, 1080p | 0.095 | ≤ 1.5 | 0.000 | ≤ 0.05 | 0.664 | ≤ 2.0 |
| High, 1440p | 0.183 | ≤ 0.6 | 0.000 | ≤ 0.05 | **0.328** (was 7.174) | ≤ 1.0 |
| Ultra, 1440p | 0.233 | ≤ 1.2 | 0.000 | ≤ 0.05 | **0.527** (was 8.789) | ≤ 1.5 |

- **High and Ultra missed the walking row 7-6x** with this document's first table (fit freeze off,
  memory 96 / 288 MB): the bench's D32 atlases nearly filled the memory row (High: 92 of 96 MB), so
  no static layer fitted, and every frame the 20 moving casters made each slot they crossed redraw
  all 2,000 static casters (`shadow` 7.0 ms at High). With room for the static layers alone
  (`JASPERENGINE_SHADOW_MEMORY_MB=192`) High went to 1.1 ms, the rest being the static layers
  redrawn as the walking camera's refitted cascades moved (`shadow static` 0.59 ms; Ultra's 4096
  cascades refitted every frame: 7.9 ms). **Fix:** High and Ultra now freeze their fits (R5, as
  Low and Medium) and their memory rows are twice their atlases, 192 and 576 MB, the change the
  spec's risks left for after AC14. A frozen fit costs up to its guard band in texel density
  (6-12 %).
- The receiver row was first measured on the full bench (2,000 pillars over the ground): about
  1.5 ms at every tier and resolution, which is per draw, not per pixel: each shadowed draw
  uploads its shadow uniform blocks (about 6 KB; the camera pass of the still bench is 2.4 ms with
  shadows, 0.5 ms without). That is outside the receiver row, and not changed here.
- These numbers are a card 4-5x a GTX 1660 (the desktop reference); the web (Iris Xe) and mobile
  (Adreno 640 / Mali-G76) rows stay to be measured on those devices.
