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
`screenshot/` (Khronos glTF-Sample-Viewer). Reviewed by the implementing agent; a human pass is
still owed (integrator note).

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
