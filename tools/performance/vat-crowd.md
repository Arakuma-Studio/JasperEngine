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

## Running it

```
python3 tools/performance/vat_crowd.py [--count 2000] [--frames 60] [--warmup 30] \
    [--cases skinned,vat_vertices,vat_bones] [--editor build/jasperengine-editor] \
    [--editor-tests build/jasperengine-editor-tests] [--bundle DIR] [--output build/performance-vat-crowd]
```

It needs the editor and `jasperengine-editor-tests` built (`-DJASPERENGINE_EDITOR_TESTS=ON`); without
a DISPLAY it runs the editor under `xvfb-run` with software GL and a D-Bus session. Results go to
`<output>/results.json` and `<output>/<count>/<case>.json` (the benchmark's own JSON).
