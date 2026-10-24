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
