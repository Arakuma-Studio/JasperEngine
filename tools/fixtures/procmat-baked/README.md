Fixture: a baked Procedural Material (docs/specs/S90-procedural-material.md AC16, AC17).

- `assets/brick_wall.procmat`: a 30-node graph (bricks, noises, a Voronoi, colorize, blend, math,
  a blur), the graph R23 to R25 are measured on (`ProcMatBudgetsGpuSlow`, docs/perf/procedural-material.md).
- `assets/brick_wall/`: what `jasperengine-editor procedural-material bake assets/brick_wall.procmat
  --size 256` wrote for it (four PNGs, their metas and `brick_wall.material`), checked in as baked.
- `Main.scene`: a sphere with that material, a camera and a sun.

`ProcMatBakedGpu` draws the material on glcore and Vulkan (golden `procmat_baked_brick_wall`), and
`SMOKE_METHOD=compile tools/smoke-test.sh tools/fixtures/procmat-baked` exports, cooks and runs it.
After a change to the node library or the bake, bake it again on a copy of this folder with the
command above, copy `assets/brick_wall/` back, and rewrite the golden (docs/testing.md).
