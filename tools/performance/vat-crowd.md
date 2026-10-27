# Vertex Animation crowd benchmark

`tools/performance/vat_crowd.py` (not gated) builds a project with N heroes on a grid 1.6 m apart
playing Walk, Idle and Wave out of sync (clip `i % 3`, a different time each), built by
`vat-crowd/scripts/Crowd.lua` when Play starts, in three scenes:

- `skinned`: one Model of `hero_animated.gltf` per hero with an Animation Player (the CPU poses
  every hero every frame; one skinned draw per hero and material),
- `vat_vertices` / `vat_bones`: one Model of the baked `.vat.glb` with an Instanced Mesh of N
  instances (culled one by one, like the skinned heroes) and a Vertex Animator playing each
  instance's clip (no CPU pose work; one instanced draw per material).

The baked hero is the end-to-end bundle's (`VatEndToEndTest`: the real baker, 30 fps, Walk + Idle +
Wave, 117 samples; Vertices: 2,799 vertices with tangent frames, 2048 x 320 RGBA16F, 5 MB; Bones:
10 bones, 2048 x 2 RGBA32F, 64 KB). Each scene is measured with `jasperengine-editor benchmark
--play` (the scene playing, as the Game view draws it; `--play` was added to the benchmark CLI for
this).

## Results (27 October 2026)

Software GL: Mesa llvmpipe under Xvfb on a shared 8-core AMD EPYC Genoa VM (other work running on
it), Game view 753 x 384, 60 measured frames after 30 of warmup. llvmpipe runs the vertex and
fragment work on the same CPUs as the game, so the numbers are only relative to each other: on a
GPU the skinned case is dominated by its CPU side (pose evaluation and one draw per hero and
material), which VAT removes entirely.

`python3 tools/performance/vat_crowd.py --count 2000,5000`

| Heroes | Case | Heroes drawn | ms / frame | FPS | Draw calls | Triangles |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 2,000 | skinned (Animation Players) | 2,000 | 1,645.8 | 0.61 | 6,000 | 9.65 M |
| 2,000 | VAT, Vertices | 2,000 | 1,042.5 | 0.96 | 3 | 9.65 M |
| 2,000 | VAT, Bones | 2,000 | 1,070.0 | 0.93 | 3 | 9.65 M |
| 5,000 | skinned (Animation Players) | 5,000 | 4,423.4 | 0.23 | 15,000 | 24.12 M |
| 5,000 | VAT, Vertices | 5,000 | 1,916.0 | 0.52 | 3 | 24.12 M |
| 5,000 | VAT, Bones | 5,000 | 1,799.7 | 0.56 | 3 | 24.12 M |

- Every case draws every hero. VAT takes 63-65% of the skinned frame time at 2,000 heroes and
  41-43% at 5,000 (1.5-1.6x and 2.3-2.5x the frame rate), with 3 draw calls instead of 6,000 /
  15,000. Vertices (4 texel fetches a vertex) and Bones (16) cost about the same here: llvmpipe's
  time goes to rasterizing the same triangles.
- The skinned crowd costs what its heroes cost, whatever the count: every Model of
  `hero_animated.gltf` draws from one set of vertex and index buffers (MeshBufferPool, the 7 buffer
  views of the hero) and the same pipelines (shared by description); per hero only its bones
  texture (OpenGL; a storage buffer on Vulkan, Metal and D3D11) is its own.
- Earlier runs of this benchmark drew only part of the skinned crowd (820 of 2,000 heroes), for two
  reasons, both fixed: `Crowd.lua` did not keep its Models, so the garbage collector destroyed most
  heroes (a Model made with `Model(scene)` owns its entity), and every skinned Model made its own
  7 GPU buffers and 6 pipelines, so sokol's buffer pool (8,192) and pipeline pool (8,192) ran out
  at ~1,170 / ~1,365 heroes, and the heroes past that silently drew nothing. A full GPU pool is now
  logged once (`GPU buffer pool full: all 16384 buffers are in use ...`) and the mesh that could
  not get its object counts as failed until the pool has room; the pool sizes are
  `Engine::GpuResourceLimits` (16,384 buffers and images, 24,576 views by default).

## Results on a GPU (27 October 2026)

NVIDIA GeForce GTX 1080 Ti (driver 580.173.02), Vulkan editor build (`-DGRAPHIC_BACKEND=vulkan`),
Release, Ubuntu 24.04, 20 cores; the editor's window on an Xvfb display (viewport 752 x 383).
`python3 tools/performance/vat_crowd.py --count 1000,2000,5000` with the default 60 frames after a
30 frame warmup. The hero is 1,608 triangles x 3 materials. Measured before the fixes above: the
skinned rows at 2,000 and 5,000 heroes drew part of the crowd only (the garbage collector and the
pool limits) and are kept for the record, not for comparison; they need measuring again.

| case | heroes | drawn | avg frame | fps | draws | triangles |
|---|---|---|---|---|---|---|
| skinned (Animation Players) | 1,000 | 1,000 | 346.38 ms | 2.9 | 3,000 | 4,824,000 |
| VAT Vertices | 1,000 | 1,000 | 7.22 ms | 138.6 | 3 | 4,824,000 |
| VAT Bones | 1,000 | 1,000 | 7.78 ms | 128.5 | 3 | 4,824,000 |
| skinned (part of the crowd, see above) | 2,000 | 820 | 281.37 ms | 3.6 | 2,460 | 3,955,680 |
| VAT Vertices | 2,000 | 2,000 | 8.17 ms | 122.5 | 3 | 9,648,000 |
| VAT Bones | 2,000 | 2,000 | 9.48 ms | 105.5 | 3 | 9,648,000 |
| skinned (part of the crowd, see above) | 5,000 | 1,023 | 927.50 ms | 1.1 | 3,069 | 4,934,952 |
| VAT Vertices | 5,000 | 5,000 | 13.75 ms | 72.7 | 3 | 24,120,000 |
| VAT Bones | 5,000 | 5,000 | 16.19 ms | 61.8 | 3 | 24,120,000 |

At 1,000 heroes a Vertex Animation crowd is ~48x faster per frame than the same heroes with Animation
Players (the skinned frame is CPU-bound: posing and uploading 1,000 skeletons, 3,000 draws), and its
cost grows little with the count (13.75 ms at 5,000 heroes, one instanced draw per material).
Vertices mode is ~10-15% faster than Bones here (2 texel fetches per vertex vs 16 for 4 influences);
Bones mode trades that for a payload a hundred times smaller.

## Running it

```
python3 tools/performance/vat_crowd.py [--count 2000] [--frames 60] [--warmup 30] \
    [--cases skinned,vat_vertices,vat_bones] [--editor build/jasperengine-editor] \
    [--editor-tests build/jasperengine-editor-tests] [--bundle DIR] [--output build/performance-vat-crowd]
```

It needs the editor and `jasperengine-editor-tests` built (`-DJASPERENGINE_EDITOR_TESTS=ON`); without
a DISPLAY it runs the editor under `xvfb-run` with software GL and a D-Bus session. Results go to
`<output>/results.json` and `<output>/<count>/<case>.json` (the benchmark's own JSON).
