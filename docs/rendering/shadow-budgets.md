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
| Shadow memory (atlases + static cache) | ≤ 16 MB | ≤ 48 MB | ≤ 96 MB | ≤ 288 MB |

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
