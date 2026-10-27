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

Software GL: Mesa llvmpipe under Xvfb on a shared 8-core AMD EPYC Genoa VM (load average 2.5-3.5
from other work during the run), Game view 752 x 383, 60 measured frames after 30 of warmup.
llvmpipe runs the vertex and fragment work on the same CPUs as the game, so the numbers are only
relative to each other: on a GPU the skinned case is dominated by its CPU side (pose evaluation and
one draw per hero and material), which VAT removes entirely.

`python3 tools/performance/vat_crowd.py --count 1000,2000`

| Heroes | Case | Heroes drawn | ms / frame | FPS | Draw calls | Triangles |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1,000 | skinned (Animation Players) | 1,000 | 800.0 | 1.25 | 3,000 | 4.82 M |
| 1,000 | VAT, Vertices | 1,000 | 451.7 | 2.21 | 3 | 4.82 M |
| 1,000 | VAT, Bones | 1,000 | 474.7 | 2.11 | 3 | 4.82 M |
| 2,000 | skinned (Animation Players) | **820** | 657.6 | 1.52 | 2,460 | 3.96 M |
| 2,000 | VAT, Vertices | 2,000 | 1,054.0 | 0.95 | 3 | 9.65 M |
| 2,000 | VAT, Bones | 2,000 | 1,067.6 | 0.94 | 3 | 9.65 M |

- At 1,000 heroes (both draw all of them) VAT takes 56-59% of the skinned frame time (1.7x the
  frame rate) with 3 draw calls instead of 3,000. Vertices (4 texel fetches a vertex) and Bones
  (16) cost about the same here: llvmpipe's time goes to rasterizing the same triangles.
- At 2,000 heroes the skinned crowd **does not draw**: only 820 heroes appear. Every skinned Model
  creates its own GPU buffers (7 for this hero) and sokol's buffer pool holds 8,192
  (`SokolSystem::setup`), so past ~1,170 heroes in a bare engine (measured in the headless test
  harness), ~820 in the editor (which holds buffers of its own), the next heroes get no buffers and
  silently draw nothing. Its frame time is for 820 heroes; per hero drawn it is 0.80 ms against
  VAT's 0.53 ms. The VAT crowd is one mesh whatever the count. (A pre-existing engine limit, not a
  Vertex Animation one: noted here, not changed.)

## Results on a GPU (27 October 2026)

NVIDIA GeForce GTX 1080 Ti (driver 580.173.02), Vulkan editor build (`-DGRAPHIC_BACKEND=vulkan`),
Release, Ubuntu 24.04, 20 cores; the editor's window on an Xvfb display (viewport 752 x 383).
`python3 tools/performance/vat_crowd.py --count 1000,2000,5000` with the default 60 frames after a
30 frame warmup. The hero is 1,608 triangles x 3 materials.

| case | heroes | drawn | avg frame | fps | draws | triangles |
|---|---|---|---|---|---|---|
| skinned (Animation Players) | 1,000 | 1,000 | 346.38 ms | 2.9 | 3,000 | 4,824,000 |
| VAT Vertices | 1,000 | 1,000 | 7.22 ms | 138.6 | 3 | 4,824,000 |
| VAT Bones | 1,000 | 1,000 | 7.78 ms | 128.5 | 3 | 4,824,000 |
| skinned | 2,000 | 820 | 281.37 ms | 3.6 | 2,460 | 3,955,680 |
| VAT Vertices | 2,000 | 2,000 | 8.17 ms | 122.5 | 3 | 9,648,000 |
| VAT Bones | 2,000 | 2,000 | 9.48 ms | 105.5 | 3 | 9,648,000 |
| skinned | 5,000 | 1,023 | 927.50 ms | 1.1 | 3,069 | 4,934,952 |
| VAT Vertices | 5,000 | 5,000 | 13.75 ms | 72.7 | 3 | 24,120,000 |
| VAT Bones | 5,000 | 5,000 | 16.19 ms | 61.8 | 3 | 24,120,000 |

At 1,000 heroes a Vertex Animation crowd is ~48x faster per frame than the same heroes with Animation
Players (the skinned frame is CPU-bound: posing and uploading 1,000 skeletons, 3,000 draws), and it
keeps drawing every hero where the skinned crowd stops at the buffer-pool limit described above.
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
