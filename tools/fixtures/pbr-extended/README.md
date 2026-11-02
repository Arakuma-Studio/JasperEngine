# pbr-extended

Materials and lighting fixtures (docs/specs/S11-materials-lighting.md). `generate.py` (see its
header) writes the benchmark project to `build/pbr-extended` (not checked in: 200 spheres are 12 MB
of scene file each) and the export fixture `tools/fixtures/pbr-extended-export`.

Benchmark scenes: `Lit Baseline`, `Lobes Off`, `Lobe Clearcoat`, `Lobe Sheen`, `Lobe Anisotropy`,
`Lobe Iridescence`, `Lobe Transmission`, `Lobe Specular`, `Lobe Hex` (200 spheres each),
`Terrain Baseline`, `Hex Terrain` (4 painted layers).

    jasperengine-editor benchmark --project build/pbr-extended --scene "Lobes Off" --out lobes-off.json

`tools/fixtures/pbr-extended-export` is the export check (AC15): runtime shader compilation off,
so a variant the export did not precompile is a missing-shader error:

    tools/smoke-test.sh tools/fixtures/pbr-extended-export
    SMOKE_BACKEND=gles3 SMOKE_GAME_RUNS="transmission PBX_SCENE=Transmission" tools/smoke-test.sh tools/fixtures/pbr-extended-export

## Review record

### AC7: the Khronos goldens (`engine/tests/render/golden/s11/`)

Written by `test_materials_pbr.py::test_khronos_models` (GL core, llvmpipe) under a grey studio
cubemap and a sun over the camera's shoulder; compared side by side with each model's
`screenshot/` (Khronos glTF-Sample-Viewer). Reviewed by the implementing agent, then by the
integrator against what each Khronos asset demonstrates (ClearCoat, Sheen, Iridescence,
Transmission and the others): approved.

| Model | Against the Sample Viewer |
|---|---|
| ClearCoatTest | layout, the coated column's extra highlight and the coat-only column match; base colours darker (no studio IBL of the same brightness) |
| SheenTestGrid | sheen colour rises along the colour axis and spreads with roughness as in the reference; the Charlie IBL is approximated (spec Risk 5) |
| AnisotropyStrengthTest / RotationTest | highlights stretch with strength and turn with rotation; the environment reflection is weaker than the reference's |
| IridescenceDielectricSpheres / MetallicSpheres | thin-film tints vary across the grid; the reference's saturated hues are softer here |
| TransmissionTest / RoughnessTest / AttenuationTest | the background refracts through the spheres, blurs with roughness, takes the attenuation colour; LDR copy (Risk 4) |
| SpecularTest / IORTestGrid | specular strength/colour and IOR rows vary as in the reference |
| EmissiveStrengthTest | the cubes brighten left to right; no bloom (none in the pipeline) |
| MetalRoughSpheresNoTextures | the metal/roughness grid matches the reference's ordering |

Each lobe-off twin (lobes disabled over MCP) differs from its golden by more than 2% of the pixels
(EmissiveStrengthTest: 1%, its cubes are 2% of the frame).

### AC12: the editor checks (`review.py`, GL core on llvmpipe, build server A under load)

| Check | Result |
|---|---|
| 1. ClearCoatTest submesh: Advanced Surface shows Clear Coat with its map | `builtin:lit#PBR_CLEARCOAT,PBR_CLEARCOAT_MAP,USE_LIT_QUALITY`, maps used `clearcoat` |
| 2. Toggle off: *Compiling* then *Ready*, no stall > 50 ms | deferred to S32 (S11 side done: job compile, fallback variant, no stale fallback); see below |
| 3. Ctrl+Z restores it | clear coat and its map back |
| 4. Three meshes, Sheen Roughness | 0.8 on all three, one undo step `Set Sheen Roughness (3 Objects)` restores 0 |
| 5. Four maps | `Extension maps 3 / 3`: clear coat, specular, transmission bound, sheen dropped |
| 6. Light Engine to Lux | 0.000% of the pixels change; with a colour temperature the colour row reads Filter |
| 7. Car Paint preset | clear coat on (and the preset's sheen) |

Check 2 in detail (`JASPER_SHADER_TIMING=1`, the editor's own frame times from `editor_status`
`frame_ms`, server A): idle frames are 190 ms median there (llvmpipe on a loaded host), so no
frame meets 50 ms. Toggling a lobe on (a variant never built before) gives one frame of 3.9 s:
the variant's GLSL/SPIR-V build runs on a job (S06, `shader-build`, Background/Heavy) while the
plain variant keeps drawing, and is swapped in when ready; the frame that swaps it pays
`sg_make_shader` (GL compile and link, 1.2-1.3 s) and llvmpipe's JIT of the program at its first
draw (the rest). Toggling back to, or onto, a variant built before costs nothing (190-215 ms,
as idle). The same 1.2 s link is paid by every new lit variant without S11 (the plain
`mesh_lit_Lq_PucShwShqNorVc4`: 1176 ms): it is the GL driver's work on the GL thread, which
sokol's GL backend and GL itself make synchronous. Moving it off the frame needs driver-side
help (a shared GL context compiling on a worker, `KHR_parallel_shader_compile`, or Vulkan
pipeline creation off the render thread): an engine-wide change outside S11, noted for the
integrator.

### AC15: exports

- `tools/smoke-test.sh tools/fixtures/pbr-extended-export` (runtime shader compilation off) passes
  on GL core, GLES3 and Vulkan: the three scenes, no missing or runtime-compiled shader; the
  transmission scene refracts the striped wall on GLES3.
- `export_check.py`: the HTML5 and Android source exports carry all 63 shaders the scenes need
  (42 of them built-in Lit lobe / Lit Quality / mobile-tier variants) in their GLSL ES 3.00
  header; the only ones left out are S10's `Shq` shadow variants, which GLES3 builds never use.

### AC11: benchmark on server E (software: llvmpipe, 4 vCPU)

`jasperengine-editor benchmark --project build/pbr-extended --scene <s> --lit-quality <q> --warmup 30
--frames 120` (editor viewport 752x362), average frame time in ms:

| Scene | Standard | High | High / Standard | vs Lit Baseline (Standard) |
|---|---|---|---|---|
| Lit Baseline | 466 | 466 | 1.001 | 1.000 |
| Lobes Off | 465 | 465 | 1.001 | 0.999 |
| Lobe Clearcoat | 465 | 467 | 1.004 | 0.999 |
| Lobe Sheen | 470 | 469 | 0.997 | 1.010 |
| Lobe Anisotropy | 465 | 465 | 1.000 | 0.998 |
| Lobe Iridescence | 468 | 471 | 1.007 | 1.005 |
| Lobe Transmission | 1119 | 1122 | 1.002 | 2.403 |
| Lobe Specular | 465 | 465 | 1.001 | 0.998 |
| Lobe Hex | 434 | 448 | 1.034 | 0.931 |
| Terrain Baseline | 16 | 16 | 0.999 | 1.000 |
| Hex Terrain | 16 | 16 | 0.998 | 1.001 |

On this software renderer the 200-sphere scenes are bound by the vertex work (238k triangles,
196 draws), which hides most fragment cost: Lobes Off is within 0.1% of Lit Baseline, the lobes
within 1%, High within 0-3.4% of Standard. Transmission costs 2.4x: every transmissive sphere
draws in the transparent queue over the scene-colour copy. Absolute and fragment-bound
numbers (the R13 ratios, hex terrain against its bound: 1.35x then, ≤ 1.75x GL / ≤ 1.4x Vulkan
at 4K since 2026-11-02) come from the GPU campaign.

### AC11 on a GPU (GPU campaign: RTX 3080 Ti, GL, 3840x2160, S15 GPU pass timers, 5 runs)

`tools/perf/gpu_campaign.py pbr --size 3840 2160`, the Game view's GPU time in ms (docs/perf/gpu-campaign.md):

| Scene | Standard | vs Lit Baseline | R13 | High / Standard |
|---|---|---|---|---|
| Lit Baseline | 0.648 | | | 1.079 (≤ 1.05) |
| Lobes Off | 0.647 | -0.2 % | ≤ +2 % | 1.082 |
| Lobe Clearcoat | 0.788 | +21.6 % | ≤ +25 % | 1.025 |
| Lobe Sheen | 0.755 | +16.5 % | ≤ +15 % | 1.103 |
| Lobe Anisotropy | 0.720 | +11.1 % | ≤ +15 % | 1.122 |
| Lobe Iridescence | 0.823 | +27.0 % | ≤ +30 % | 1.075 |
| Lobe Specular | 0.705 | +8.8 % | ≤ +3 % | 1.054 |
| Lobe Transmission | 1.982 | +205.9 % (scene-colour copy included) | ≤ +20 % excluding it | 1.047 |
| Lobe Hex | 1.222 | +88.6 % | ≤ 3x the hexed maps' taps | 1.025 |
| Terrain Baseline | 1.173 | | | 1.038 |
| Hex Terrain | 6.056 | 5.16x the terrain | ≤ 1.35x | 1.012 |

Over: Sheen, Specular/IOR and High quality by a few points, and the hex terrain by 4x (open).
