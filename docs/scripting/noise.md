# Noise in Lua

`Noise` and `NoiseGraph` give scripts the noise library and Noise Graphs. They are in the `noise` player
module: a project whose scripts name them carries it automatically, and a project that only bakes
terrain pays nothing.

## Noise

Static functions with the same bits as the kernels on every target. An omitted seed is 0.

```lua
local a = Noise.perlin(1.5, 2.5, 7)
local b = Noise.perlin3(1.5, 0.25, 2.5, 7)
local c = Noise.simplex(1.5, 2.5)
local d = Noise.value(1.5, 2.5, 7)
local e = Noise.cellular(1.5, 2.5, 7, "f2_minus_f1")
local f = Noise.fbm(1.5, 2.5, 7, 6, 2.0, 0.5, "perlin")
local g = Noise.ridged(1.5, 2.5, 7, 6)
```

`Noise.perlin`, `simplex`, `value` give -1..1. `cellular` gives a distance, or -1..1 for `cell_value`.
`fbm` takes `(x, z [, seed, octaves, lacunarity, gain, basis])` and has the basis' range. `ridged` is 0..1.

## NoiseGraph

`NoiseGraph.load(path)` returns a compiled graph, or nil after logging the issues. A graph with an erosion
or Terrain Height node does not load in a game: bake those in the editor.

```lua
local graph = NoiseGraph.load("graphs/Hills.noisegraph")
if graph then
    print(graph.isGridOnly, #graph.outputs)
    graph:setVariable("roughness", 0.6)
    local low, high = graph:rangeOf("height")
    local h = graph:sample(0.25, 0.75, 7)
    local v = graph:sample3(0.25, 0.5, 0.75, 7)

    local grid = graph:sampleGrid{x = 0, z = 0, width = 64, depth = 64, step = 1 / 64, seed = 7}
    print(grid.width, grid.depth, grid.min, grid.max, grid.hash, grid:get(3, 4))

    local task = graph:sampleGridAsync{width = 256, depth = 256, step = 1 / 256, seed = 7}
    -- later, in an update:
    if task.done then
        local done = task:result()
    end
    task:cancel()
end
```

A grid has at most 4,000,000 points. Its samples sit at texel centres, `(round(x / step) + i + 0.5) * step`,
so a grid gives the same bits a generation gives at those texels. `sample` on a graph with a grid-only
node, such as Slope Mask, is an error that names the node: use `sampleGrid`. `sampleGridAsync` runs on
small jobs, keeps the frame, and works without worker threads. A task's `result()` is nil until `done`.
