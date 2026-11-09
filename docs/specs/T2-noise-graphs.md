# T2 — Noise graphs and terrain generation

Wave 5 (terrain programme, spec-first). Binding decisions: `TERRAIN-LEDGER.md` D1-D21, cited here as
D1, D4 ...; this spec's own decisions are N1 ... N17. Reports: `/tmp/terrain-research/reports/`
(`fastnoise2.md`, `voxel-meshing-generators.md` section 3, `our-terrain-authoring.md`,
`engine-constraints.md`), checked against the code where cited. Seams: T1 owns the writes (D4), T6
reads this library (D6).

## 1. Goal

Anyone, a person or an agent, can make a believable terrain in seconds from a seed and a preset, then
open the recipe, change it and see every step of it. The recipe is a **Noise Graph**: a small,
readable `.noisegraph` file, compiled to a plan over buffers and evaluated by our own noise library,
which gives the same bits on all six export targets. Generation runs as a job, writes through T1's
operations as one undo step, and can paint layers and foliage density from masks in the same graph.
Games that bake pay nothing. Games that want noise at run time turn on one small player module, which
T6's voxel volumes also use.

## 2. Scope

**In scope**
- `engine/core/noise/`: basis kernels (Perlin, Simplex, Value, Cellular; 2D and 3D), fractals, batch
  APIs, deterministic math, golden hashes.
- The Noise Graph asset, its 29 node types, its compiler (plan, dead nodes, constant folding, interval
  analysis, per-node profile) and its evaluator.
- Hydraulic and thermal erosion as heightfield stages (`engine/core/terrain/HeightfieldErosion`).
- `generate_terrain` and `preview_noise_graph`; `.noisegraph` as the first real graph kind of
  `edit_graph`, `validate_graph` and `get_graph`.
- Ten built-in presets shipped as `.noisegraph` files; the seam T1's Create Terrain dialog uses.
- `GraphCanvas` ports, typed pins and curved wires (additive); the Noise Graph window; the Terrain
  window's Generate section.
- Lua `Noise` and `NoiseGraph`, the C++ runtime evaluator, the `JASPERENGINE_NOISE` player module.
- Replacing `editor/util/TerrainNoise.h`, with its outputs preserved (N13).

**Out of scope**
- FastNoise2 at run time or in the editor (D10), its encoded trees and its node editor.
- Compute or GPU noise: the global decisions (`docs/specs/README.md:16-20`) and `CLAUDE.md` rule out
  requiring compute, and CPU bakes are enough at these budgets. An 8-wide AVX2 path (the curl precedent,
  `CurlNoiseBatch.h:27-32`) is not part of this spec; a later one must keep the goldens.
- Sub-graphs, expression nodes, comments and group drawing. The file keeps groups (`Graph.h:86-92`);
  the window does not draw them yet.
- A holes output: T3 owns what a hole means (D7). A later `output_hole` node is an additive change.
- Scattering rules, tree placement and impostors (T5), tiles and streaming (T4), meshing (T6).
- Rewriting the Erode brush (`editor/util/TerrainErosion.*`, which T1 R22 moves to `engine/core/terrain/`),
  which stays a brush and keeps its output.
- Erosion at run time: the compiler refuses it in games (N7).
- The curl noise, the camera shake Perlin and the cloud bake (R37).

## 3. Current state

Every citation was read on 2026-11-04 at `b4f7816ea`. Nothing was built or run for this spec, so all
behaviour below is inferred from reading, except what is marked as a grep.

**Terrain noise today**
- `editor/util/TerrainNoise.h:11-12`: value noise on an integer lattice, with `hash` (`:16`), `value`
  (`:28`) and `fractal` (`:45`). It lives in an editor header, without contraction control.
- It has **three** callers, not the two the reports name: the Noise brush
  (`editor/window/TerrainEditWindow.cpp:33,1849`), `set_terrain_maps`' `fractal_noise`
  (`editor/mcp/TerrainMaps.cpp:12,65`) and the Erode brush's droplet starts
  (`editor/util/TerrainErosion.cpp:6,79-80`).
- `set_terrain_maps`' `random_noise` draws from `std::mt19937` and `std::uniform_real_distribution`
  (`TerrainMaps.cpp:50-51,56`). The distribution's output is implementation-defined (C++ standard), so
  that mode differs across standard libraries. An omitted seed is `std::random_device`
  (`editor/mcp/tools/WorldTools.cpp:708-709`).
- Tests use `fractal_noise` with a seed and read heights back but pin no hash
  (`tools/mcp/tests/test_world.py:97`, `test_splines.py:70`).
- Whole-map generation exists only through that tool. People have the Noise brush and an icon-only
  "+" that makes a flat map (`TerrainEditWindow.cpp:2465-2467`).
- The terrain samples texel centres at `(i + 0.5) / size` (`engine/core/subsystem/MeshSystem.h:340-344`),
  while `fractal_noise` uses `x / (resolution - 1)` (`TerrainMaps.cpp:63`). New generation follows the
  renderer.
- Maps are at most 4096 per side (`WorldTools.cpp:703`). `terrain.maxHeight` is in world units and
  the heightmap is normalized (`engine/core/component/TerrainComponent.h:191-192`). A terrain has up
  to 9 layers in 3 RGB blend maps (`TerrainComponent.h:21-24`).

**How bit-identical SIMD works here**
- `engine/core/math/simd/SimdFloat4.h:7-21`: each back end (Scalar, Sse2, Neon, Wasm) is a struct of
  static functions, and a kernel is a template compiled per back end. There are no estimates and no
  FMA, `min` and `max` use SSE's operand order, and `floor` is exact.
- `engine/core/particles/ParticleSources.cmake:34-48` sets the per-file flags: `-ffp-contract=off`,
  `/fp:precise`, `-msimd128` on the web, `JASPERENGINE_SIMD_DISABLE` for `JASPERENGINE_WASM_SIMD=OFF`.
- `GradientCurl3D.h:36` wraps the lattice every 256 cells so the float-to-integer conversion stays
  defined; `GradientCurl3DSimd.h:66` is the formula.
- `engine/tests/core/particles/CurlNoiseTest.cpp:107-131` compares Scalar and Native bit for bit in
  one process. No cross-target hash is in core tests: the web gate holds a state hash
  (`tools/web/gates/particles.json`).
- Contraction has already bitten us: Recast on arm64 (`engine/CMakeLists.txt:1255-1261`) and the mip
  chain (`:2147-2151`).
- `armeabi-v7a` and `x86` ship (`engine/workspaces/androidstudio/app/build.gradle:51-54`). `SimdFloat4.h:14`
  notes armv7's missing vector divide but not that ARMv7 NEON flushes denormals (general ARM
  behaviour, inferred), which breaks bit identity with Scalar.
- Grep of `engine/core`, `editor`, `engine/CMakeLists.txt` and `engine/cmake`: nothing sets
  `_MM_SET_FLUSH_ZERO`, `fesetenv` or `-ffast-math`. Jolt's `FPFlushDenormals` (a class in
  `engine/libs/joltphysics/Jolt/Core/FPFlushDenormals.h`) has no user in `Jolt/**/*.cpp`.

**Graph model and tools (S23)**
- `engine/core/graph/Graph.h:7-12` is generic JSON with passthrough. Nodes have a string `type` and
  named `params` (`:60-68`); `Graph.cpp:400-422` runs migrations, and a newer `kindVersion` is kept
  with a warning. `graph::save` is deterministic (`Graph.h:140-142`).
- `GraphRegistry.h:55-66`: `GraphKindDesc{id, fileExtension, kindVersion, migrations, acyclic, nodeTypes,
  portTypes, validate}`. It is compiled for every platform (`S23:286-287`).
- `editor/mcp/tools/GraphTools.cpp:230-288`: the five tools, in group `scripting`
  (`editor/mcp/tools/McpTools.cpp:64`). The only kind is the env-gated test kind (`GraphTools.cpp:36-72,226`).
- **A gap the reports miss:** no tool can create a graph file. `readGraph` fails on a missing file
  (`GraphTools.cpp:85`), and `write_file` refuses unknown extensions (`tools/mcp/tests/test_graph.py:39`).
  `GraphEditCmd::undo` writes `before` back (`editor/command/type/GraphEditCmd.cpp:28-30`), so it cannot
  undo a creation.
- `editor/window/widget/GraphCanvas.h:5-12,27-48`: boxes and straight arrows from node to node, with
  no ports. The only caller is the Animator (`editor/window/AnimatorWindow.cpp:1396-1501`), probed as
  `animator.canvas` (`:1502`).

**Jobs, modules**
- `engine/core/jobs/JobSystem.h:18-25`: jobs never touch sokol, Lua or ImGui, and with 0 workers nothing
  runs at submit. `jobs::GateClass::Heavy` is `JobTypes.h:30-33`; the web budget is 4 ms of Background per
  frame (`S06:250-253`). `MainThreadQueue` does not drain while paused (`engine/core/thread/MainThreadQueue.h:14-19`).
  `engine/core/graph/*` includes only its own folder, `Export.h` and json; `engine/core/jobs` also needs
  `Log.h`, `thread/ThreadUtil.h` and `memory/MemoryStats.h` (both by grep).
- `bake_navigation` (`editor/mcp/tools/NavigationTools.cpp:125-240,826-838`) is the job precedent:
  `wait`, `timeout_seconds`, a cancel handler, `replyWhenJobDone` (`editor/mcp/McpJobs.h:117`).
- A module (S34 R1, `S34:73-88`) touches `engine/CMakeLists.txt:42,2055`,
  `engine/core/player/PlayerModules.{h:17,cpp:13,19-35}`, `engine/player/variants.json`,
  `engine/cmake/JasperPlayerVariants.cmake:12-17`, `editor/players/ModuleUsage.cpp:43`,
  `editor/Exporter.cpp:3589-3598,4611` and `tools/lua_api/extract.py:48-52`.
- `Exporter::copyAssets` ships every file under the assets directory (`editor/Exporter.cpp:2753-2800,3022`),
  so a `.noisegraph` would ship today.
- The asset table is `editor/asset/AssetTypes.cpp:30-55`; `.particles` is the sidecar precedent (`:40`).
- There is no CI: upstream removed the GitHub workflows (`297562c95`), so nothing runs after a push. Section 6's matrix says
  what runs here and what a person runs elsewhere. Android emulator tests need KVM (`docs/testing.md:223`), which this host lacks.
- `generate` and `preview` are in `VERBS` (`tools/mcp/tests/test_tool_conventions.py:37-41`), which
  `our-terrain-authoring.md` left unverified. `tools/mcp/budget.json` has no `terrain` entry in
  `groups`, so `group_default` applies (15,000 characters, 12 tools) until D17's raise.
- `TerrainEditorSettings` is project-level and already persisted
  (`editor/Project.h:542`, `editor/Stream.cpp:1846,1879`). Built-in templates embed at configure time
  (`CMakeLists.txt:403-407`, `editor/pose/PoseTemplates.h:9-10`).

## 4. Reference

Nothing is vendored. `NOTICE.md` gains no entry, and `engine/notices.json` none (it covers `engine/libs` only), unless an implementer
ports a specific FastNoise2 algorithm, which this spec does not ask for (MIT; it would need a kept header and an entry). The
do-not-copy list (`docs/specs/README.md:23-36`) does not touch this work. Upstream dropped the README's "Licences" bullet with its
MPL-2.0 line; the README now says only that copied code keeps its header and gets a `NOTICE.md` entry. That lifts nothing here:
**OpenEngine's erosion filter (MPL-2.0) stays banned by ledger D14: do not open it.**

| Source | Take | Leave |
|---|---|---|
| Perlin, "Improving Noise" (2002); Gustavson, "Simplex noise demystified" (2005); Worley, "A Cellular Texture Basis Function" (1996) | The algorithms, written from the papers | Any published code |
| Musgrave, Kolb, Mace, "The Synthesis and Rendering of Eroded Fractal Terrains" (1989) | Thermal erosion: slopes above a talus shed downhill | |
| Beyer, "Implementation of a method for hydraulic erosion" (2015 thesis) | Droplets with inertia, capacity, deposition and evaporation | Sequential order: ours must run in parallel with the same bits (N6) |
| Quilez, domain warping article | Domain warp as `p + amp * fbm(p)` | |
| `FastNoise2` (MIT; report and `FN2/src/...`), read as data | Grid positions derived from integers; batch APIs; the list of what breaks determinism: `-ffast-math` (`src/CMakeLists.txt:92-94`), `std::cos` rotation constants (`include/FastNoise/Generators/Modifiers.h:84-86`), `uint8_t` registration-order ids (`include/FastNoise/Metadata.h:175`) | Everything else (D10) |
| `godot_voxel` (MIT module; report section 3), read as data, ideas only (D14) | One function per node for compile, buffer and range (`generators/graph/node_type_db.h:91-98`), range readouts on nodes (`doc/source/performance.md:270`), per-node profile, live previews | Its code, its uint16 stream, its Godot types, FastNoise2 as a leaf with a crude `±1` range |
| Our `SimdFloat4`, `CurlNoiseBatch`, `ParticleSources.cmake`, `tools/web/gates/particles.json` | The back-end pattern, the flags, the gate | The curl code itself, which has its own wrap and fields |

## 5. Requirements

### Functional — the noise library (`engine/core/noise/`, namespace `jasperengine::noise`, D1, D6)

- **R1. Kernels.** Perlin, Simplex, Value and Cellular, each 2D and 3D. A kernel is one template over a
  back end of `SimdFloat4.h` (N1). The `Scalar` instantiation is the reference; `Sse2`, `Neon` and `Wasm`
  give the same bits.
- **R2. Batch APIs.** Position arrays (`const float* x, y, z`, any count) and uniform grids (2D and
  3D). A sample is a pure function of its position, seed and parameters, so where a sample falls in a
  block or a chunk cannot change its bits.
- **R3. Single point.** `noise::perlin2(x, z, seed)` and its siblings, on a one-lane back end
  (`NoiseBackend.h`) so a call does not compute four lanes. Bit-identical to the batch kernels.
- **R4. Bit identity.** For every kernel and every node, Scalar equals Native bit for bit, in one
  process (R41) and against one golden table on every target.
- **R5. No contraction, no libm in kernels.** Allowed: `B::` operations, `B::floor` and `B::sqrt`
  (both exact in IEEE 754). Banned in kernel and plan code: `std::sin`, `cos`, `tan`, `atan*`, `pow`, `exp*`,
  `log*`, `fmod`, `round`, `<random>`, `long double`, `std::hash`. `NoiseDetMath.h` provides polynomial
  `sin`, `cos`, `tan`, `exp2`, `log2` and `pow`, on one-lane and 4-wide back ends. It is checked against
  libm within tolerance in a test and pinned by goldens.
- **R6. Numerics.**
  - Coordinates wrap at 65536 cells with the curl formula, then clamp into `[0, 65536)`, so every float
    to integer conversion is in range on every back end.
  - A non-finite coordinate is treated as coordinate 0.
  - Divide by zero gives 0, the square root of a negative gives 0, and `pow` of a negative base gives 0.
  - Every node output is sanitised (non-finite to 0) unless interval analysis proves it finite. NaN
    payloads differ between x86 and ARM, so a NaN must never reach a stored bit.
- **R7. Threads.** The library owns no thread, holds no mutable global and takes no lock while evaluating.
  Parallel work goes through `jobs::parallelFor` and `jobs::submit`. Results do not depend on the worker
  count, and it works with 0 workers (web without pthreads).
- **R8. Fractals.** FBm is `Σ w_i n_i(p f_i) / Σ w_i`, with `f_{i+1} = f_i * lacunarity`,
  `w_{i+1} = w_i * gain` (iterated, never `pow`) and seed `seed + i * 1013`. Ridged is
  `r_i = (1 - |n_i|)^2`, weighted by the previous octave's `r` clamped to 0..1, normalized to 0..1.
  Octaves 1..12.

### Functional — node set

- **R9. Node types.** Exactly these 29, registered under stable string ids. Each has ports, parameters,
  a compile function, a range function and at least one golden case:

  | Category | Type id (Name) | Inputs | Parameters | Output and range |
  |---|---|---|---|---|
  | Input | `position` (Position) | | | `coord`, fields `x`, `y`, `z`; the domain |
  | Input | `terrain_uv` (Terrain UV) | | | fields `u`, `v`; 0..1 over the evaluated domain |
  | Input | `constant` (Constant) | | `value` | field; folded |
  | Input | `variable` (Variable) | | `name` | field; the graph variable, folded when exposed values are set |
  | Input | `terrain_height` (Terrain Height) | | | field 0..1; the terrain's current heights; bake-only |
  | Noise | `perlin`, `simplex`, `value` | `coord?` | `dimensions` 2d/3d, `frequency`, `seed_offset` | -1..1 |
  | Noise | `cellular` | `coord?` | as above + `metric` euclidean/manhattan/chebyshev, `return` f1/f2/f2_minus_f1/cell_value, `jitter` | by `return` |
  | Fractal | `fbm` (FBm) | `coord?` | `basis`, `dimensions`, `frequency`, `octaves`, `lacunarity`, `gain`, `seed_offset` | the basis' range |
  | Fractal | `ridged` (Ridged) | `coord?` | as FBm | 0..1 |
  | Domain | `domain_warp` (Domain Warp) | `coord?`, `strength?` | `basis`, `dimensions`, `frequency`, `amplitude`, `octaves`, `seed_offset` | `coord` |
  | Domain | `domain_transform` (Domain Scale/Offset) | `coord?` | `scale`, `offset`, `rotation_y` | `coord` |
  | Math | `scale_offset` (Scale/Offset) | `in` | `scale`, `offset` | `in * scale + offset` |
  | Math | `math` (Math) | `a`, `b?` | `op` add, subtract, multiply, divide, min, max, power, modulo, abs, negate, one_minus, sqrt, floor, frac, sin, cos; `b` when unconnected | by `op` |
  | Math | `clamp` (Clamp) | `in` | `min`, `max` | |
  | Math | `remap` (Remap) | `in` | `from_min`, `from_max`, `to_min`, `to_max`, `clamp` | |
  | Math | `curve` (Curve) | `in` | `keys` as S20's `[[t, v, in, out], ...]` | LUT of 256, linear between, min and max of the LUT |
  | Math | `terrace` (Terrace) | `in` | `steps` 2..64, `smoothness` 0..1 | |
  | Math | `select` (Select) | `a`, `b`, `mask` | `threshold`, `falloff` | union of `a` and `b`; a proven mask removes a branch |
  | Math | `blend` (Blend) | `a`, `b`, `weight?` | `weight` | union of `a` and `b` |
  | Mask | `height_mask` (Height Mask) | `in` | `min`, `max`, `falloff` | 0..1 |
  | Mask | `slope_mask` (Slope Mask) | `in` | `min_angle`, `max_angle`, `falloff` (degrees) | 0..1; grid-only |
  | Stage | `hydraulic_erosion` (Hydraulic Erosion) | `height`, `hardness?` | R24 | `height`, `flow`, `sediment`; grid-only, bake-only |
  | Stage | `thermal_erosion` (Thermal Erosion) | `height` | R25 | `height`; grid-only, bake-only |
  | Output | `output_height` (Height Output) | `in` | `unit` normalized/metres | at most one |
  | Output | `output_layer` (Layer Output) | `weight` | `layer`, `optional` | one per layer |
  | Output | `output_density` (Foliage Density Output) | `density` | `foliage_layer`, `optional` | one per foliage layer |
  | Output | `output_value` (Value Output) | `in` | `name` | one per name; for scripts and T6 |

- **R10. Typed ports.** Two port types, `field` (one float per point) and `coord` (x, y, z per point),
  with no implicit conversion. An unconnected `coord?` means `position`. An unconnected field input
  with a same-named parameter uses the parameter.
- **R11. Seeds.** A node's seed is `root seed + seed_offset`, modulo 2^32, and depends on nothing else: not
  node ids, order or position. Adding or removing a node leaves every other node's bits unchanged.

### Functional — the Noise Graph asset

- **R12. File.** `.noisegraph` is the `Graph.h` JSON format: `kind: "noise"`, deterministic save, two-space
  indent, unknown keys kept at every level. Nodes and parameters are keyed by name, never by position
  (N3). `metadata.noise.space` is `terrain` (coordinates in terrain widths, the default) or `world`
  (metres). Exposed `variables` are the graph's public knobs.
- **R13. Identity.** `AssetType::NoiseGraph = 28` (reserved, D19: pixel art holds 24-27; the value is stored in `assetmap.bin`, so it never moves) is a sidecar asset under the assets base, in `editor/asset/AssetTypes.cpp`.
  It gets a GUID through S04, moves and renames keep it, and it appears in the Project window with an icon.
- **R14. Versions.** `formatVersion` follows S23; `kindVersion` starts at 1 with a migrations list. A
  shipped node's parameter defaults and its output are frozen, by a golden of the node-type table and
  by the hash goldens. A new behaviour is a new parameter whose default keeps the old output, or a
  new type id. An unknown node type is kept, reported, and makes compile fail on that node; a newer
  `kindVersion` loads with S23's warning and compiles only the nodes it knows.
- **R15. First real kind.** The `noise` kind is registered at editor start and, in a game, at the first
  `NoiseGraph.load`. `list_graph_kinds`, `list_graph_node_types`, `get_graph`, `validate_graph` and `edit_graph`
  work on it without kind-specific code in `GraphTools.cpp`. Kind-specific validation codes:
  `no-output`, `duplicate-output`, `grid-only-node`, `bake-only-node`, `octave-limit`, `erosion-budget`,
  `unknown-variable`, `curve-keys`.
- **R16. Creating a graph.** `edit_graph` gains `create: {kind, from_preset?}`, which makes a missing
  file (empty, or a copy of a preset) and applies `ops` to it, as one undo step. Undo removes the file.
  An existing file is refused. It is one parameter, because a file a tool cannot create is a kind a tool
  cannot start.

### Functional — compiler and evaluator

- **R17. Plan.** `noise::compile(graph)` produces an immutable `CompiledGraph`: a flat list of ops over
  numbered buffers, with buffer slots reused once a value's last reader has run. Evaluation runs op by op
  over chunks of at most 4096 points, never per sample through a virtual call. The compiled form is
  never stored; it is derived data (N5).
- **R18. Dead nodes and folding.**
  - Nodes that reach no output are removed.
  - Nodes whose inputs are all constant are evaluated at compile time.
  - A `select` or `blend` whose mask or weight is proven to one side drops the other branch.
  - The report names every dead and folded node.
- **R19. Interval analysis.** Every node has an analytic `[min, max]`, computed from its inputs' ranges
  and shown in the preview. It must contain every value the node outputs (tested by sampling).
  `CompiledGraph::rangeOver(box)` returns an output's range over a position box, for T6's block
  clipping.
- **R20. Profile.** An optional per-evaluation profile returns milliseconds, nanoseconds per point and
  share of the total, per source node. When off it adds no clock call.
- **R21. Evaluators.** `evaluateGrid` (2D, global integer texel start and units per texel, R23),
  `evaluateGrid3D` (samples on the lattice, `float(double(start + i) * unitsPerVoxel)` with no half step, because T6 R5
  needs a coarse sample to be the fine sample at its position) and `evaluatePoints` (for T6 and scripts), plus `compile` flags `gridOnly` and
  `bakeOnly`. A `CompiledGraph` may be evaluated from many threads at once, each with its own `EvalScratch`.
- **R22. Stages.** Grid-only nodes run over a whole region buffer and split the plan into segments. The
  compiler computes the padding a region needs (slope 1 texel; thermal `iterations`; hydraulic
  `lifetime + radius + 2`), evaluates the padded region, and crops. Peak memory is reported.
- **R23. Seams.** Position is `float((double(start + i) + 0.5) * unitsPerTexel)` per axis, from a 64-bit
  global texel index. So a graph without erosion gives the same bits for a texel whichever region,
  tile or resolution-equal call evaluates it, including `slope_mask` thanks to padding. This is what
  makes T4's tiles seamless without knowing about noise. Erosion is the exception (R27).

### Functional — erosion

- **R24. Hydraulic.** `terrain::HeightfieldErosion::hydraulic`: droplets from Beyer's model. Parameters and
  defaults: `droplets_per_texel` 0.25 (0.01..4), `lifetime` 48 (8..128), `inertia` 0.05, `capacity` 4,
  `min_slope` 0.01, `erode_rate` 0.3, `deposit_rate` 0.3, `evaporation` 0.02, `gravity` 4, `radius` 1 (1..4),
  `seed_offset`. An optional `hardness` field scales the cut. Outputs: `height`, `flow` (where water
  ran, 0..1) and `sediment` (where material settled, 0..1), so a graph can paint riverbeds and scree.
- **R25. Thermal.** `thermal`: `iterations` 8 (1..64), `talus_angle` 35 degrees (through `NoiseDetMath`
  `tan`), `rate` 0.5. It is a symmetric pair flux computed from a read-only copy each iteration, so row order
  and thread count do not matter and it conserves material up to float rounding.
- **R26. Bounded cost.**
  - `erosion-budget` is a validation error when `droplets * lifetime` exceeds 2^31 or
    `texels * iterations` exceeds 2^34 at the requested resolution.
  - Work is checked for cancellation at least once per tile (at most 50 ms of work between checks at
    budget).
  - A run reports its droplet steps. Horizontal and vertical scale come from the evaluation context
    (texel size, `maxHeight`), so any resolution erodes alike in world terms.
- **R27. Determinism.** Same seed and parameters give the same bits on any worker count and any target
  (N6). A region smaller than the terrain erodes with padding but is not guaranteed to match a
  whole-map run; `generate_terrain` warns `erosion-region-seam`.

### Functional — generation

- **R28. One path.** `editor/noise/TerrainGeneration` is what the Generate section, the Create Terrain
  dialog and `generate_terrain` call: validate, snapshot what the graph reads, evaluate on a job
  (`Lane::Background`, `GateClass::Heavy`), and apply on the main thread.
- **R29. Writes (D4, D6).** Every write goes through T1's `terrain::Batch` (`TerrainOps.h`, T1 R88): one undo step for the whole
  generation, whichever maps it touches, named `Generate Terrain (Mountains, seed 7)`, reporting the
  dirty rectangle. No PNG writing here: T1 owns persistence, collider and foliage refresh.
- **R30. Layers and density.** A graph's `output_layer` nodes paint by rules: weights are clamped to 0..1;
  when they sum above 1 they are normalized; layer 0 takes the rest unless it has its own output.
  `output_density` writes a foliage layer's density. A missing layer or foliage layer is an error, or a
  warning and a skip when the node says `optional` (presets do).
- **R31. Region and resolution.**
  - `region` is terrain-local metres, snapped outward to texels and reported.
  - `feather` (metres) lerps new values over old across the region's border, using the snapshot.
  - `resolution` applies to dry runs at any value, and to real runs only on a terrain without a height
    map or for a whole-terrain run (which replaces the map at that resolution by T1's structural path, T1 R1, R28). A partial run
    at another resolution is refused.
  - Clipping outside 0..1 is counted, not an error.
- **R32. All-or-nothing (D15).** Graph errors, a missing terrain, a bad region and missing required layers
  are reported before any evaluation, with the issues and their node ids in `structuredContent`.
- **R33. Job behaviour.** `wait` and `timeout_seconds` as in `bake_navigation`. Cancel leaves the terrain
  untouched. If a graph that reads `terrain_height` finds the terrain changed between snapshot and apply,
  the job fails with `terrain-changed` and writes nothing; a graph that does not read it applies. Refused
  in Play.

### Functional — presets

- **R34. Built-ins.** `editor/resources/noisegraphs/*.noisegraph`, embedded at configure time like the
  pose templates, listed by `editor::TerrainPresets`:
  `rolling_hills`, `mountains`, `alpine` (mountains with erosion and snow, rock and scree layers),
  `canyons`, `islands`, `dunes`, `plateaus`, `plains`; and two utility graphs, `erode_existing` and
  `paint_by_slope`, which read `terrain_height` and are hidden from the Create dialog. Each file's
  `metadata.preset` holds its id, name, description, tags and recommended `terrain_size` and `max_height`.
- **R35. Character.** Each terrain preset has statistics a generated map must meet over seeds 1, 2 and 3
  at 257 texels on a 1000 m terrain (N14), and a screenshot in the evidence, so a preset cannot silently
  stop looking like its name.

### Functional — replacing `TerrainNoise.h` (N13)

- **R36.** `editor/util/TerrainNoise.h` is deleted. Its three callers call `noise::compat::hash01`,
  `value` and `fractal`. On a build without contraction these return exactly what the header
  returned, for every input. `set_terrain_maps`' `fractal_noise` and the Noise and Erode brushes keep
  their output for the same seed.
- **R37.** Unchanged, not moved and not reimplemented: `engine/core/math/Noise.h` (camera shake),
  `engine/core/render/AtmosphereNoise.cpp` (periodic cloud bake), `engine/core/math/noise/*` (curl).
  `random_noise` is not T2's: T1 R19 moves it to a seeded per-texel hash.

### Platform & backends

- **R38. Flags.** `engine/core/noise/NoiseSources.cmake` applies the particle flags to every source of
  the library: `-ffp-contract=off` or `/fp:precise`, `-msimd128` on the web, `JASPERENGINE_SIMD_DISABLE` when
  `JASPERENGINE_WASM_SIMD=OFF`. Kernel headers are included only by those files, so no inline kernel is
  compiled without the flags. `HeightfieldErosion.cpp` gets `-ffp-contract=off` too. The Xcode project lists
  the files and gives them the flag (`tools/noise/xcode_sync_noise.py --check`, as
  `tools/particles/xcode_sync_particles.py`).
- **R39. armv7 runs Scalar.** On `armeabi-v7a`, `noise::Native` is `simd::Scalar`: ARMv7 NEON flushes
  denormals, VFP does not (N2). All other targets use their widest `SimdFloat4` back end.
- **R40. Guards.** `static_assert(FLT_EVAL_METHOD == 0)` and `std::numeric_limits<float>::is_iec559` in the
  kernels' translation unit. They catch a 32-bit x86 build on x87, which `x86` Android must not be.
- **R41. Goldens on every target we can run.** One table,
  `engine/tests/core/noise/NoiseGoldenTable.inc`: FNV-1a 64 over the raw float bytes of each case (every
  node type, each basis in 2D and 3D, every preset at 64 texels, erosion, tiny and huge coordinates, one
  case per target-sensitive numeric rule of R6). The table runs in three places: `NoiseGolden.*` in
  core tests, the standalone `jasperengine-noise-golden` (`tools/noise/golden/`: a Release build of only
  noise, graph and the table, with `JASPERENGINE_NOISE_ALL_BACKENDS` and `JASPERENGINE_NOISE_SERIAL`, N17), and
  the `noise` web gate project. See the matrix in section 6.
- **R42. Module.** `JASPERENGINE_NOISE` (CMake option, default ON; module name `noise`) wires up as
  S34 R1 requires: the `NoiseSources.cmake` list, `JASPERENGINE_NOISE=$<BOOL:...>`, `allModules`, `builtModules`,
  `moduleDisplayName`, `variants.json`, `JasperPlayerVariants.cmake`, `Exporter.cpp`'s `moduleSwitch` and its
  Xcode branch, `_MODULE_MACROS`, and `lib_size.py`'s groups. OFF compiles every module file to nothing.
  The editor always builds it ON (root CMake errors otherwise). `noise::compat` is outside the module
  (N13). `tools/players/lib_size.py` must not count curl noise as this module: curl shares the namespace.
- **R43. Detection.** `ModuleUsage` needs `noise` when a Lua script names `Noise` or `NoiseGraph` (found
  from the Lua API's `modules`), on dynamic access or C++ scripts, as S34 R5 says; T6 adds Voxel Volume
  components. A `.noisegraph` in the assets is not evidence by itself, and neither is a baked terrain.
- **R44. Variants.** `noise` joins `full`, `ui` and `core`, and stays out of `lite3d` and `lite2d` (N11).
- **R45. Bakes pay nothing.** When `noise` is not needed, `copyAssets` skips `*.noisegraph`, the Lua
  bindings and every kernel are absent, and a Compile build has no `noise` symbol. When it is needed, every
  `.noisegraph` ships, because a Lua string can name any of them.

### Performance

Budgets are targets set from reasoning (no code exists), on `min(8, threads - 2)` workers of a Release
build with an otherwise idle machine, gated by `je-bench-noise` and `tools/perf/check_bench.py`. The
first measurement may lower a number by at most 30 %, recorded in Deviations with the measurement;
beyond that the code is profiled.

- **R46. Kernels** (Mpts/s, one thread, x86-64 SSE2, 4-wide):

  | Kernel | x86-64 SSE2 | Scalar (any) | Web simd128 | Web scalar | arm64 mobile (target) |
  |---|---|---|---|---|---|
  | `value` 2D | 40 | 13 | 20 | 8 | 20 |
  | `perlin` 2D | 30 | 10 | 15 | 6 | 15 |
  | `simplex` 2D | 20 | 7 | 10 | 4 | 10 |
  | `cellular` 2D, f1 | 6 | 2 | 3 | 1.2 | 3 |
  | `value` 3D | 20 | 7 | 10 | 4 | 10 |
  | `perlin` 3D | 14 | 5 | 7 | 3 | 7 |
  | `simplex` 3D | 10 | 3.5 | 5 | 2 | 5 |
  | `cellular` 3D, f1 | 1.5 | 0.5 | 0.75 | 0.3 | 0.75 |

  armv7 runs Scalar on a slow core: the target is 10 % of the x86-64 column. Single-point calls take at
  most 60 ns (Perlin 2D). Web numbers are gated only on an idle machine (`?noise_bench=1`, as the
  particles project does) and otherwise only reported. The arm64 and phone columns are targets: unverified here (section 7).
- **R47. Whole maps.** `mountains` without erosion (12 octaves in all):
  1024² at most 1.2 s on one thread and 0.4 s on the workers; 4096² at most 4 s on the workers.
- **R48. Erosion.** Hydraulic at least 8 M droplet steps/s per thread; thermal at least 40 M texel
  iterations/s per thread. `alpine` at 1024², total, at most 2 s on the workers.
- **R49. Compile and preview.** Compile: 64 nodes at most 1 ms, 1,000 nodes at most 20 ms. All node
  previews of a 32-node graph at 64² (no erosion) at most 16 ms on one worker. `S23` R36 still holds for load
  and save.
- **R50. Editor and disabled cost.** With the Noise Graph window closed and the Generate section collapsed,
  no job, no texture and no per-frame work. With it open: at most 1 ms per frame on the main thread for a
  64-node graph, at most 8 thumbnail uploads per frame. Registering the kind allocates only the
  descriptors, at editor start or a game's first load.
- **R51. Player size (`noise` on).** At most 0.20 MiB on Linux glcore, 0.16 MiB on Android arm64, 0.16 MiB
  on web `st` (60 KiB gzipped), by `tools/players/lib_size.py --modules`. Players carry only the Native
  back end, and the armv7 and scalar web builds carry only Scalar: the second instantiation exists only under
  `JASPERENGINE_NOISE_ALL_BACKENDS`, set for editor and test targets. OFF: 0 bytes in the module's group.
- **R52. Memory.** A chunk's scratch is `buffers * 4096 * 4` bytes per worker, with no allocation per chunk
  once warm. Peak for a generation is at most `live grid buffers * texels * 4 + 64 MiB`, and `peak_bytes` is
  in the result. At 4096² that is about 67 MiB per live grid.

### Editor UX (human)

- **R53. Canvas extension.** `GraphCanvas` gains, additively: per-node input and output pins (label, colour
  by port type, connected and error state), wires as cubic curves between pins, drag from a pin to a
  compatible pin, drag to empty space (the caller opens node search filtered by that pin), wire selection,
  a node thumbnail (`ImTextureID`), footer lines and an error line. Incompatible pins dim during a drag
  through a caller-supplied `canConnect`.
- **R54. It must not disturb Animator.** A node without pins and a link without pin ids take today's code
  path. The Animator's call site and its tests do not change.
- **R55. Noise Graph window.** `editor/window/NoiseGraphWindow`, menu Window > Terrain > Noise Graph,
  and a double-click on a `.noisegraph`. It has:
  - a toolbar: Graph (asset picker), Seed with a dice button, Preview Size, Space (Terrain Size or World
    Units), Frame, Preview, Apply;
  - the canvas;
  - a right panel: the selected node's parameters, or the exposed variables and the Preview image with
    its statistics.
- **R56. Live feedback.** Each node shows a 64-pixel thumbnail of its output, a range readout (`-0.62 ...
  0.71  ≤ ±1`, observed then analytic) and its time. While a parameter is dragged, previews update from
  the working value on a background job, and stale results are dropped. A dead node is dimmed and a folded
  node is marked. Thumbnails hide below zoom 0.5. A coord output shows as two channels.
- **R57. Undo.** Every gesture is one `GraphEditCmd` in the project history, named by `graph::describe`
  (`Connect Perlin → Remap`, `Delete 3 Nodes`). A parameter drag is one step, committed on release. Copy,
  paste, duplicate and delete work with Ctrl+C, Ctrl+V, Ctrl+D and Delete through the shortcut catalog (S25).
  An external edit of the file (MCP, a text editor) refreshes the window; a file that fails to parse
  shows the error and refuses edits instead of overwriting it.
- **R58. Errors on the node.** Every `graph::Issue` with a node id draws on that node: a red outline, the
  message in the node, the port highlighted. Issues without a node show in a bar above the canvas. Nothing
  goes to the Console for a graph error. Apply is disabled while errors remain, and its tooltip names the
  first one.
- **R59. Node search.** Right-click, Tab, or a wire dropped on empty space opens one search popup: by name
  and category, filtered by the pin's type if any, Enter adds and connects. A node added from a dropped wire
  connects to the first compatible port.
- **R60. Generate section.** A collapsible **Generate** section in the Terrain window (a new file with a
  one-line call, D2): Noise Graph (a preset list and an asset picker), Seed with a dice button, the
  graph's exposed variables, Apply To (Height, Layers, Foliage Density), Region (Whole Terrain or Rectangle,
  Feather), Preview (no write: a hillshaded image, min, max, slope histogram), Apply, a progress bar and
  Cancel. The last graph and seed persist in `TerrainEditorSettings` (`generateGraph`, `generateSeed`,
  with defaults, and in `check_stream_keys.py`'s table).
- **R61. Create Terrain seam.** T1's Create Terrain dialog lists `TerrainPresets::list()` and, when one is
  chosen, calls `TerrainGeneration::start` after creating the map, with the preset's recommended size and
  height as defaults. T1 owns the dialog; T2 owns those two calls.
- **R62. Asset integration.** Project window > Create > Noise Graph makes a graph from a chosen preset
  (Blank has a single `output_height` and a Constant); `window.noiseGraph` in the command catalog.

### Agent UX (MCP, CLI)

- **R63. `generate_terrain`** (group `terrain`, a job, D15, section 6). `graph` or `preset`, `seed` (default 1,
  always echoed), `variables`, `region`, `feather`, `resolution`, `apply_to`, `dry_run`, `max_width`, `wait`,
  `timeout_seconds`. The result has `hash` per output, `stats` (height min, max, mean, p5, p50, p95 in
  normalized units and metres; slope histogram; `clipped_low`, `clipped_high`; per-layer coverage; per-density
  coverage), `changed` (T1 R9's `[{map, rect, local, texels}]`), `undo_name`, `plan` (ops, dead, folded, peak_bytes), `ms`
  (evaluate, erosion, apply) and `warnings`. A dry run writes nothing and returns a heightmap image.
- **R64. `preview_noise_graph`** (group `terrain`, read-only, a job with `wait`, allowed while playing). `path` or
  `preset`, optional `ops` (the same as `edit_graph`, applied in memory), `seed`, `size` (16..512, default 128),
  `extent`, `images` (`none`, `outputs`, `all`; at most 12), `max_width`. Per node: `id`, `type`,
  `range_analytic`, `range_observed`, `mean`, `hash`, `ms`, `ns_per_point`, `share`, `dead`, `folded`, `issues`.
- **R65. Discoverability.** `docs/mcp/guide/noise-graphs.md` (`jasper://docs/noise-graphs`) holds the node
  table, a worked example, the loop (`edit_graph`, `preview_noise_graph`, `generate_terrain dry_run`, real run,
  `sample_terrain`) and the fact that `edit_graph` lives in group `scripting` while calls reach every tool. The agent guide gets no
  line for it: `generate_terrain`'s description points at the doc, and `terrain.md` (T1 R73) at both. A graph error's result names
  the fix in `next` in `structuredContent` (`edit_graph` on that node), since that is what a client shows the model.
  `list_graph_node_types` carries ranges of parameters from `ParamDesc`.
- **R66. Budget.** `generate_terrain` at most 1,400 characters (1,249 by script over N12), `preview_noise_graph` at most 1,200 (883),
  both with no `exceptions` entry and within the `terrain` group limits of T1 R74 (D17, `tools/mcp/budget.json`). Adding `create` to
  `edit_graph` keeps it and `scripting` within their limits. Tool test, hidden-until-enabled test and regenerated `docs/mcp/TOOLS.md`.

### Lua and runtime (module `noise`, D18)

- **R67. `Noise`.** Static functions on the main thread: `Noise.perlin(x, z [, seed])`, `perlin3(x, y, z [, seed])`,
  and the same for `simplex`, `value`, `cellular` (+ `cellularReturn`); `Noise.fbm(x, z [, seed, octaves, lacunarity,
  gain, basis])`, `Noise.ridged(...)`. Same bits as the kernels. They are not bound at all in a build without the
  module; `ModuleUsage` makes that a non-issue for projects that use them.
- **R68. `NoiseGraph`.** `NoiseGraph.load(path)` returns a graph or `nil` plus logs the issues. Properties
  `outputs`, `isGridOnly`. `graph:setVariable(name, value)`, `graph:rangeOf(output)`,
  `graph:sample(x, z [, seed, output])`, `graph:sample3(x, y, z [, seed, output])`,
  `graph:sampleGrid{x, z, width, depth, step, seed, output}` returns a `NoiseGrid` (`.width`, `.depth`, `.min`,
  `.max`, `.hash`, `:get(i, j)`), at most 4 M points. `graph:sampleGridAsync{...}` returns a task (`.done`,
  `.progress`, `:result()`, `:cancel()`), split so that one job stays under 1 ms and web without pthreads keeps
  its pump budget. `bakeOnly` nodes fail to load in a game; `sample` on a grid-only graph is an error
  naming the node.
- **R69. C++ runtime API.** `engine/core/noise/NoiseGraphRuntime.h` (section 6) is T6's seam: compile from
  a `graph::Graph`, `evaluateGrid3D`, `evaluatePoints`, `rangeOver`, thread-safe with an `EvalScratch` per thread.

### Data & compatibility

- **R70. Old projects.** No scene, prefab or component format change. `TerrainEditorSettings` gets two
  defaulted keys. A project without a `.noisegraph` behaves exactly as today, except `noise::compat`
  replacing the header with the same outputs.
- **R71. Docs.** `docs/terrain-generation.md` (people), `docs/scripting/noise.md` and `docs/mcp/guide/lua-api.md`
  (Lua, doctested), `docs/mcp/guide/noise-graphs.md`, `docs/perf/noise.md` (measurements), `docs/testing.md` rows
  (the golden tool, the gate), `docs/mcp/TOOLS.md` regenerated.

### Upstream revision: no CI, lean MCP, Doriax

- **R72. Self-check on any platform.** There is no CI, so the golden tool is what a person runs where this host cannot. It takes
  `--all-backends` and `--report <file>|-`, prints `noise-golden <target> <backend> <cases> pass|FAIL table=<hash>` per back end, names
  each failing case, and exits 0 on pass and 1 on any mismatch. `--self-test` flips one expected hash and must exit 1. From the repository root:

  | Platform | Command |
  |---|---|
  | Windows x64, with `vcvars64` | `cmake -S tools/noise/golden -B build/noise-golden -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl && cmake --build build/noise-golden && build\noise-golden\jasperengine-noise-golden.exe --all-backends --report noise-golden-windows.txt` |
  | macOS arm64 | `cmake -S tools/noise/golden -B build/noise-golden -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/noise-golden && build/noise-golden/jasperengine-noise-golden --all-backends --report noise-golden-macos.txt` |
  | macOS x86_64 | The same with `-DCMAKE_OSX_ARCHITECTURES=x86_64`, run as `arch -x86_64 build/noise-golden/jasperengine-noise-golden --all-backends --report noise-golden-macos-x64.txt` |
  | iOS Simulator | `cmake -S tools/noise/golden -B build/noise-golden-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 && cmake --build build/noise-golden-ios --config Release && xcrun simctl spawn booted build/noise-golden-ios/Release-iphonesimulator/jasperengine-noise-golden --all-backends --report -` |
  | iOS device | Export `tools/test-projects/noise` for iOS, run it from Xcode; its script compares every preset's hash with the table and logs `noise_selfcheck=pass table=<hash>` (the `noise` web gate expects the same line, AC48) |

  These commands are written from CMake's options and the removed CI jobs, and none was run here (*assumed* until a person does). The person pastes the report into `docs/perf/noise.md`'s determinism record. Until a platform has a row there it is reported
  **unverified**, and a mismatch is a kernel bug (section 9), never a new table.
- **R73. Tool names and shape.** `generate_terrain` and `preview_noise_graph` follow the gated `verb_noun` rule (`docs/mcp/conventions.md`,
  `VERBS` in `test_tool_conventions.py`), not `AGENTS.md`'s "area noun first", which no gate enforces. Both were challenged against merging
  (T1 R95): `preview_noise_graph` could fold into `generate_terrain dry_run`, but it needs no terrain, works in Play and reports per node, so
  it stays. `preset` is a string the handler checks; schemas carry no field or result lists.
- **R74. Eval cases.** Each new tool and its text gets a case in `tools/mcp/eval/cases.py` that fails without it (AC49).
- **R75. Doriax (D21).** Nothing in T2 duplicates a Doriax capability. `agent-authoring` adds `edit_terrain_details` and restructures
  the guide; `output_density` writes density by a details index, and R65's doc is its own file. Whichever lands second merges. No shader
  changes here, so no variant bit and no shader cache bump.

## 6. Design constraints & decisions

**N1. Kernels are templates over `SimdFloat4.h`; no new SIMD layer.** Additions to `SimdFloat4.h`, if any, are
new functions only: particles must not recompile to different code. `NoiseBackend.h` adds `Lane1` (one float,
the same static API over the subset kernels use) and defines `noise::Native` (R39). Existing `jasperengine::noise`
curl names stay; the new code must not collide with them (`Curl3`, `curlNoiseBatch`, `gradientcurl3d`).

**N2. armv7 is scalar on purpose.** A gradient kernel can make denormals near a lattice point (`t^5` of a tiny
fraction, times a tiny difference), and ARMv7 NEON would flush what VFP keeps. Snapping small fractions would
distort the output, so the rule is: no NEON on that ABI. It loses about two thirds of the speed (assumed) on an ABI
now rare, and it gets the same bits. arm64 NEON honours denormals by default, and no code here sets a flush mode
(section 3). `NoiseDeterminism.FloatingPointEnvironmentIsDefault` probes it.

**N3. `.noisegraph` is the S23 JSON, not YAML.** The model, tools, undo and determinism exist. Example:

```json
{
  "formatVersion": 1,
  "kind": "noise",
  "kindVersion": 1,
  "nextId": 6,
  "nodes": [
    {"id": "n1", "type": "fbm", "position": [0.0, 0.0],
     "params": {"basis": "perlin", "dimensions": "2d", "frequency": 4.0, "octaves": 6,
                "lacunarity": 2.0, "gain": 0.5, "seed_offset": 0}},
    {"id": "n2", "type": "remap", "position": [240.0, 0.0],
     "params": {"from_min": -1.0, "from_max": 1.0, "to_min": 0.0, "to_max": 1.0, "clamp": true}},
    {"id": "n3", "type": "output_height", "position": [480.0, 0.0], "params": {"unit": "normalized"}}
  ],
  "edges": [
    {"id": "e4", "from": {"node": "n1", "port": "out"}, "to": {"node": "n2", "port": "in"}},
    {"id": "e5", "from": {"node": "n2", "port": "out"}, "to": {"node": "n3", "port": "in"}}
  ],
  "variables": [], "groups": [], "viewport": {"offset": [0.0, 0.0], "zoom": 1.0},
  "metadata": {"noise": {"space": "terrain"}}
}
```

**N4. Coordinates.** `terrain` space divides the world by the whole terrain or world width, so a preset fits any
terrain size. `world` space is metres, for things that must keep a physical scale. The position formula is R23;
float precision at `terrain` scale is ample, and in `world` space a coordinate up to about 130 km keeps
about 1/64 m (`2^17` with a 24-bit mantissa).

**N5. No cooked binary form.** Games parse the JSON and compile at load: 64 nodes in at most 1 ms (R49) does not
justify a second format with its own versioning. S23 already ships `Graph.cpp` in games.

**N6. Parallel erosion with the same bits: tile colouring.** Droplets are sequential by nature, so:
- The grid is cut into square tiles anchored at the global origin, with side `2 * (lifetime + radius + 2)`.
  A droplet starting in a tile cannot reach a same-coloured tile.
- A run has 4 rounds, each with a grid offset from `hash(seed, round)` (to avoid tile-aligned artefacts) and a
  quarter of the droplets. Each round runs 4 colours in order, and the tiles of one colour in parallel.
- Each tile's droplets are in a fixed order, with start positions from `hash(seed, round, tileX, tileZ, index)`.
- Droplet code is scalar, with `-ffp-contract=off` and `std::sqrt` only.

The result does not depend on which thread runs which tile. Thermal is Jacobi (R25).

**N7. Erosion is a bake.** A droplet pass is seconds of work at map scale, and the web's single-thread pump gives
4 ms a frame (`S06:253`). A game that wants eroded ground bakes it. `bakeOnly` nodes (erosion, `terrain_height`)
fail at load in a game with `bake-only-node`. The code still links when the module is on, because the tests and
the editor share it; it costs about 8 KiB (assumed; measured under R51).

**N8. D1 and the module.** `HeightfieldErosion` lives in `engine/core/terrain/` per D1, in `jasperengine::terrain`,
with no ECS or render dependency, but is compiled only with `JASPERENGINE_NOISE` so bakes pay nothing.

**N9. Generate writes with T1's absolute operations.** T2 calls T1 R88's route and nothing else:

```cpp
// engine/core/terrain/TerrainOps.h; T1's names win
terrain::Batch batch(store);
batch.readHeights(rect, out);                                       // the terrain_height snapshot
batch.setHeights(rect, std::span<const float> normalized);          // absolute, 0..1
batch.setLayerWeights(rect, layers, weights);                       // normalized per R30
batch.setDensity(rect, foliageLayer, std::span<const float>);
TerrainOpResult result = batch.commit();                            // dirty rects, clipped counts
// the editor wraps the commit as one TerrainMapDeltaCmd, "Generate Terrain (Mountains, seed 7)"
```

**N10. Previews never touch the main-thread queue.** `NoisePreviewService` submits a Background job per graph
revision, and the window polls `Task::isReady`, because `MainThreadQueue` stops while paused. A newer revision cancels
the older (`CancelToken`). `NoiseGraphDocument` holds all window logic without ImGui, so it unit-tests headless.

**N11. Variants.** `noise` is in `full`, `ui` and `core`; not in `lite3d` or `lite2d`.
- A Lua `Noise` user on the prebuilt flow lands on `core` (22 MiB against lite3d's 18, `S34:407`) rather
  than `full` (34 MiB).
- A baked project on a lite variant pays exactly nothing. A baked project that needs `ui` or `core` for other
  reasons carries up to 0.20 MiB (0.6-0.9 % of the file) in the prebuilt flow, and nothing in a Compile build.

S34 already has variants carrying unused modules. A new `lite3d` plus noise variant is not worth a runtime per
platform.

**N12. Tool shapes.** Built with `McpSchema.h`; both tools are in `editor/mcp/tools/TerrainGenerationTools.cpp`, a new
module row in `McpTools.cpp`'s table (group `terrain`). Schemas are lean, and the handler validates.

```text
generate_terrain {scene?, entity? (T1's terrain ref), graph?, preset? (string), seed=1, variables? (anyObject name -> number),
  region? (anyObject {min:[x,z], max:[x,z]}), feather=0, resolution?, apply_to? (array of strings),
  dry_run=false, max_width=512, wait=true, timeout_seconds=120}
  "Generate a Terrain from a Noise Graph or preset as one undo step (a job). dry_run writes nothing and returns statistics and a
   heightmap image. wait false returns job_id for get_job. Default seed 1, always echoed. Node graphs: jasper://docs/noise-graphs."

preview_noise_graph {path?, preset? (string), ops?, seed=1, size=128, extent=1, images=outputs, max_width=256,
  wait=true, timeout_seconds=60}
  "Evaluate a Noise Graph without writing: per-node ranges, ms, dead and folded nodes, output hashes and images. ops previews edits
   as edit_graph would make them. Needs no terrain and works in Play. To see a terrain's result use generate_terrain dry_run."

edit_graph: + create? {kind, from_preset?}
```

`allowedWhilePlaying`: `generate_terrain` false, `preview_noise_graph` true. Annotations: `generate_terrain`
`destructive`, `preview_noise_graph` `readOnly`, `idempotent`. Activity as `bake_navigation`.

**N13. Old outputs: preserved, in one place.** `engine/core/noise/NoiseCompat.{h,cpp}` holds `hash01`, `value` and
`fractal` as the old header wrote them, without the lattice wrap, built with the noise flags. The old header on
x86-64 (no FMA) already computes these bits, so that is the output contract, pinned by a table captured from the
old header **before** it is removed (`docs/testing.md` "characterization tests"). An old arm64 editor may have
contracted (inferred from `engine/CMakeLists.txt:1255-1261`); it now joins the x86-64 values, and that one narrow
change is accepted because maps are stored PNGs and nothing regenerates them.

It compiles **outside** the module (under 1 KiB) because T1 may move the brushes into `engine/core/terrain/`, which
exists in players. The `value` node equals `compat::value * 2 - 1` on `[0, 65536)` and an FBm of `value` equals
`compat::fractal`, so the library's definitions are the old ones extended, not a second algorithm.

**N14. Preset character tests** (`NoisePresets.Character`, 257 texels, 1000 m, seeds 1, 2 and 3), thresholds fixed on
first landing from measured values, in these shapes:

| Preset | Statistic |
|---|---|
| `plains` | p95 - p5 under 0.1, slope p95 under 8 degrees |
| `rolling_hills` | p95 - p5 in 0.15..0.5, slope p95 under 25 degrees |
| `mountains` | p95 - p5 over 0.4, slope p95 over 30 degrees |
| `alpine` | as mountains, and `flow` p99 over 10 times its median |
| `canyons` | height histogram bimodal, slope p99 over 45 degrees |
| `islands` | every border texel under 0.02, centre region mean over 0.2 |
| `dunes` | directional variance ratio over 3 along the wind axis |
| `plateaus` | at least 4 histogram peaks |

**N15. Runtime header.**

```cpp
// engine/core/noise/NoiseGraphRuntime.h
namespace jasperengine::noise {
    struct GridDomain { int64_t startX = 0, startZ = 0; int width = 0, depth = 0;
                        double unitsPerTexel = 1.0; double y = 0.0; };
    struct Grid3Domain { int64_t startX = 0, startY = 0, startZ = 0; int width = 0, height = 0, depth = 0;
                         double unitsPerVoxel = 1.0; };
    struct EvalContext { uint32_t seed = 1; float maxHeight = 100.0f; double metresPerTexel = 1.0;
                         std::span<const std::pair<std::string, float>> variables;
                         const float* terrainHeight = nullptr;      // the snapshot, when the graph reads it
                         jobs::CancelToken* cancel = nullptr; bool parallel = true; NodeProfile* profile = nullptr;
                         EvalScratch* scratch = nullptr; };   // null: the evaluator's own; T6's jobs pass theirs
    struct Interval { float min, max; };
    class EvalScratch;                              // per thread, R21; size from scratchBytes()

    class JASPERENGINE_API CompiledGraph {          // immutable: share across threads
    public:
        bool isGridOnly() const; bool isBakeOnly() const; int outputIndex(std::string_view name) const;
        Interval rangeOf(int output) const;
        Interval rangeOver(int output, const float boxMin[3], const float boxMax[3]) const;
        size_t scratchBytes() const;
        bool evaluateGrid(const GridDomain&, const EvalContext&, int output, float* out) const;
        bool evaluateGrid3D(const Grid3Domain&, const EvalContext&, int output, float* out) const;
        // one thread, no allocation: T6's blocks are already on a job
        void evaluatePoints(const float* x, const float* y, const float* z, size_t count, const EvalContext&,
                            int output, float* out, EvalScratch& scratch) const;
    };
    struct CompileResult { std::shared_ptr<const CompiledGraph> graph; std::vector<graph::Issue> issues;
                           std::vector<std::string> deadNodes, foldedNodes; };
    JASPERENGINE_API CompileResult compile(const graph::Graph&, CompileOptions = {});
    JASPERENGINE_API void registerNoiseKind();      // idempotent; GraphRegistry owns the descriptors
    JASPERENGINE_API uint64_t hashFloats(const float* values, size_t count);   // FNV-1a 64 of the raw bytes
}
```

**N16. Canvas additions.**

```cpp
struct Pin { uint64_t id = 0; std::string label; ImU32 color = 0; bool connected = false; bool error = false; };
struct Node { /* existing fields */ std::vector<Pin> inputs, outputs; ImTextureID thumbnail{}; ImVec2 thumbnailSize;
              std::vector<std::string> footer; std::string errorText; };
struct Link { /* existing fields */ uint64_t fromPin = 0, toPin = 0; };   // 0: node to node, as today
struct Result { /* existing fields */ uint64_t wireFrom = 0, wireTo = 0, wireDroppedFrom = 0; ImVec2 wireDropPosition; uint64_t hoveredPin = 0; };
std::function<bool(uint64_t fromPin, uint64_t toPin)> canConnect;  // null: anything connects
```

**N17. The golden tool has no engine.** All parallel loops in the library and in erosion go through one helper,
`noise::detail::parallelChunks`. `JASPERENGINE_NOISE_SERIAL` turns it into a plain loop, for the tool only: it needs
no job system (and so no `Log`, `ThreadUtil` or `MemoryStats`), and it links in seconds under any toolchain, including the
NDK's static bionic. Worker-count independence is proven by AC2 and AC16 in the core tests, which run the real helper.

**Where things live** (new files, D2):

| Path | What |
|---|---|
| `engine/core/noise/` | `NoiseSources.cmake`, `NoiseBackend.h`, `NoiseDetMath.h`, `NoiseKernelImpl.h`, `NoiseKernels.{h,cpp}`, `NoiseCompat.{h,cpp}`, `NoiseNodes.{h,cpp}`, `NoisePlan.h`, `NoiseCompiler.cpp`, `NoiseRanges.cpp`, `NoiseEval.cpp`, `NoiseGraphRuntime.h` |
| `engine/core/terrain/` | `HeightfieldErosion.{h,cpp}` |
| `engine/core/script/binding/` | `NoiseClassesLua.cpp` |
| `editor/noise/` | `NoiseGraphDocument`, `NoisePreviewService`, `TerrainGeneration`, `TerrainPresets` |
| `editor/window/` | `NoiseGraphWindow`, `TerrainGenerateSection` |
| `editor/mcp/tools/` | `TerrainGenerationTools.cpp` |
| `editor/resources/noisegraphs/` | the ten presets |
| `tools/noise/` | `golden/` (standalone CMake, `main.cpp`), `cross_check.sh`, `check_kernels.py`, `xcode_sync_noise.py` |
| `tools/test-projects/noise/`, `tools/web/gates/noise.json` | the web and Android test project |
| tests | `engine/tests/core/noise/`, `engine/tests/core/terrain/`, `editor/tests/noise/`, `editor/tests/widget/GraphCanvasTest.cpp`, `tools/mcp/tests/test_noise_*.py`, `test_generate_terrain.py` |

Existing files touched, smallest hooks only:
- `TerrainEditWindow.cpp`: the include and call at `:33,1849`, one call for the Generate section.
- `TerrainErosion.cpp:6,79-80`, `TerrainMaps.cpp:12,65`: the swap.
- `GraphTools.cpp` and `GraphEditCmd`: `create`.
- `GraphCanvas.{h,cpp}`: the additions.
- The module wiring of R42, `AssetTypes.cpp`, `Project.h` and `Stream.cpp` (two keys), `App.cpp`,
  `DefaultCommands.cpp` (the menu row), `ProjectWindow.cpp` (Create), `McpTools.cpp`, root `CMakeLists.txt`.
- No Docker image and no workflow: `docker/android.Dockerfile` has no qemu-user, and this spec does not add it (see below).
- `MeshSystem.cpp`, `RenderSystem.cpp`, `InspectorWindow.cpp`, `WorldTools.cpp`: **nothing**.

**Cross-target determinism: the matrix.** All compare against the one table of R41. A difference is a bug in the
kernel or its flags: never regenerate the table for one target, and never add a per-target table.

| Target | Back end | Compiler | How | On this host |
|---|---|---|---|---|
| Linux x86-64 | Sse2, Scalar | GCC 11 | `tools/build.sh unit-tests` (`NoiseGolden.*`), golden tool | **checked** |
| Web | Wasm simd128, Scalar (`WASM_SIMD=0`) | emsdk 6.0.10, V8 | `WEB_PROJECT=noise tools/build.sh web`, `GATE=noise tools/build.sh web-gate`, both twice, in headless Chrome (SwiftShader) | **checked** |
| Android arm64-v8a | Neon A64 | NDK clang | `tools/noise/cross_check.sh`: the tool under `qemu-aarch64-static` (user mode, no KVM) | only if the Android image has qemu-user; it has none today, so **unverified** |
| Android armeabi-v7a | Scalar (R39) | NDK clang | the same under `qemu-arm-static` | as above: **unverified** |
| Android x86_64, x86 | Sse2 | NDK clang | `cross_check.sh`: the tool, static bionic, run natively | checked if a static bionic binary starts here (*inferred*); else **unverified** |
| Windows x64 | Sse2, Scalar | `cl` | R72 | **not verifiable here**, unverified until run |
| macOS arm64 | Neon A64, Scalar | Apple clang | R72, in Release, since Debug would not exercise the optimizer | **not verifiable here**, unverified until run |
| macOS x86_64 | Sse2 | Apple clang | R72 under Rosetta | **not verifiable here**; *inferred* from NDK x86_64 clang |
| iOS arm64 | Neon A64 | Apple clang | R72, Simulator or device | **not verifiable here**; *inferred* from macOS arm64 |
| Any Android on a device | | | On a host with `/dev/kvm`: `ANDROID_PROJECT=noise ANDROID_ABIS=x86_64 tools/build.sh android`, then `tools/android-emulator-test.sh` | **not verifiable here** |

`tools/noise/cross_check.sh` runs in the Android image (`tools/build.sh exec android tools/noise/cross_check.sh`). It builds the tool for
the four ABIs with the release flags of the player (`-O2`, ThinLTO, the noise flags), static, and runs each it can. A row it cannot run
prints `UNVERIFIED (no qemu-user in the image)` and never counts as a pass. `--red-contract` builds it with `-ffp-contract=fast -mfma`
on x86-64 and must fail. `python3 tools/noise/check_kernels.py` is a lint `tools/check-changed` does not run: run it for this area.

## 7. Acceptance criteria

Unit tests live in `jasperengine-core-tests` (`engine/tests/core/...`) or `jasperengine-editor-tests`
(`editor/tests/...`). Every MCP test file declares `CI = True` (the default MCP suite's marker) and runs on its own project. There is no
CI: gates are `tools/check-changed` per commit and the full suite before the push (`AGENTS.md`, Test). A result this host cannot produce is
labelled unverified, with R72's command, in the report.

- **AC1. Scalar and Native.** `NoiseDeterminism.ScalarMatchesNativeBitForBit`: every kernel, 2D and 3D, 10,000 random
  points including negative cells, the wrap and tiny coordinates, compares `uint32` bits. `NoiseKernels.SinglePointMatchesBatch`.
  → R1, R3, R4
- **AC2. Chunking.** `NoiseKernels.ChunkingDoesNotChangeBits`: the same array split at every offset 0..7, tails included;
  `NoiseEval.WorkerCountDoesNotChangeBits`: the same hashes with 0, 1, 3 and 7 workers. → R2, R7
- **AC3. Goldens.** `NoiseGolden.AllCasesMatch` (fails when a case is missing from the table);
  `NoiseNodes.EveryTypeHasAGoldenAndARange` (fails for a registered type with no case). Run: `tools/build.sh unit-tests`.
  → R4, R9, R41
- **AC4. Numerics.** `NoiseKernels.ExtremeInputsStayDefined` (±1e30, NaN, ±Inf, denormals, -0) and
  `NoiseEval.NoNonFiniteReachesAnOutput`, both under `SANITIZE=address,undefined tools/build.sh unit-tests` with
  `-fsanitize=float-cast-overflow`. → R6
- **AC5. Math and lint.** `NoiseDetMath.CloseToLibm` (tolerance table) and `NoiseDetMath.Golden`.
  `python3 tools/noise/check_kernels.py --self-test` fails on a planted `std::pow`, `std::thread` or a noise source
  missing from `NoiseSources.cmake`, and the real tree passes. → R5, R7, R38
- **AC6. Threads.** `NoiseThreads.CompiledGraphIsShareable` under `SANITIZE=thread tools/build.sh unit-tests`;
  `NoiseThreads.ZeroWorkersRuns`. → R7, R21
- **AC7. Fractals.** `NoiseKernels.FbmDefinition`, `RidgedDefinition`: against a slow reference written in the test from
  R8. → R8
- **AC8. Ports and seeds.** `NoiseGraphAsset.CoordDoesNotFeedField` (type-mismatch issue),
  `NoiseGraphAsset.AddingANodeKeepsOtherOutputs` (hash of other nodes unchanged after adding and reordering). → R10, R11
- **AC9. File.** `NoiseGraphAsset.RoundTripIsByteIdentical`, `.UnknownKeysSurvive`, `.UnknownNodeIsKeptAndFailsOnThatNode`,
  `.MigrationRunsOnce` (a test kind copy at version 2), `.NewerKindVersionWarns`, `NoiseNodes.TableIsFrozen` (golden of
  `toJson(kind)`; additions allowed, changes fail). `AssetTypes.NoiseGraphIsSidecar` in editor tests. → R12, R13, R14
- **AC10. Graph MCP.** `test_noise_graph.py::test_kind_with_existing_tools`: kind listed; 29 node types; `edit_graph`
  connect and setParam is one undo step; `validate_graph` returns each kind code from a fixture; `get_graph` keeps unknown
  keys. `test_generate_terrain.py::test_create`: `create` makes a file; a second create is refused; undo removes the file; `from_preset` copies.
  → R15, R16
- **AC11. Compile.** `NoiseCompile.DeadNodesAreRemoved`, `.ConstantsFold`, `.ProvenMaskDropsABranch`, `.BuffersAreReused` (peak
  below node count for a chain), `.ReportsDeadAndFolded`. → R17, R18
- **AC12. Ranges.** `NoiseRanges.ObservedWithinAnalytic`: every node type over 3 seeds and 100,000 points;
  `NoiseRanges.RangeOverBoxContainsSamples`. → R19, R69
- **AC13. Profile.** `NoiseCompile.ProfileCoversLiveNodes`; `NoiseCompile.ProfileOffCallsNoClock` through a counting
  clock seam in the test. → R20
- **AC14. Evaluators.** `NoiseEval.PointsMatchGrid`, `.Grid3DMatchesPoints`, `.Grid3DCoarseIsFineDecimated`, `.GridOnlyRefusesPoints`, `.BakeOnlyRefusedWhenForbidden`.
  → R21, R22, R69
- **AC15. Seams.** `NoiseEval.SubRegionMatchesWholeBitForBit`, `.AdjacentTilesAgreeAtAnyOrigin` with a start of 2^40
  texels, `.SlopeMaskIsSeamless`. → R23
- **AC16. Erosion bits.** `HeightfieldErosion.SameBitsForAnyWorkerCount` (0, 1, 3, 7) and `.GoldenHash`, on all targets through
  the table. Red control: a scratch patch makes two colours run together, and the test goes red. → R24, R27, N6
- **AC17. Erosion behaviour.** `HeightfieldErosion.ConservesMaterial`, `.CarvesChannels` (flow p99 against median, and
  curvature falls), `.HardnessSlowsTheCut`, `.ResolutionScales`; `.ThermalRespectsTalus`, `.ThermalConserves`,
  `.ThermalAnyThreadOrder`; `.CancelStopsWithinATile`; `.RefusesOverBudget` (`erosion-budget`, also through `validate_graph`).
  Screenshots of `alpine` with and without erosion in the evidence. → R24, R25, R26
- **AC18. Provenance.** Review: the implementer's report states that no OpenEngine erosion file was opened; the new sources
  carry only our header; `python3 tools/ci/check-notices.py` passes; `NOTICE.md` is unchanged unless a port is recorded.
  → section 4
- **AC19. Dry run.** `test_generate_terrain.py::test_dry_run_writes_nothing`: returns stats and an image; file hashes of
  the maps and the undo history unchanged; `hash` equal between two runs. → R63, R28
- **AC20. Apply and undo.** `test_generate_terrain.py::test_apply_is_one_undo_step`: heights change, `sample_terrain` matches stats, one undo step
  named as R29; undo restores byte-identical maps; redo gives the same `hash`. `TerrainGeneration.WritesOnlyThroughOps` (a
  fake `TerrainMapStore` records every write). → R28, R29
- **AC21. Layers and density.** `test_generate_terrain.py::test_layers_and_density`: `paint_by_slope` on a sculpted terrain; coverage matches
  `sample_terrain`; `TerrainGeneration.WeightsNormalize`, `.OptionalLayerWarnsAndSkips`, `.RequiredLayerFailsBeforeEvaluating`.
  `test_generate_terrain.py::test_all_or_nothing`: an invalid graph, a bad region and a missing terrain fail, with issues and node
  ids in `structuredContent`, before evaluation (no job time) and with nothing written. → R30, R32
- **AC22. Region.** `test_generate_terrain.py::test_region_and_feather`: only the snapped rectangle changes; the sub-rectangle's values equal a
  whole run's; feather lerps; a partial run at another resolution is refused. → R31
- **AC23. Job.** `test_generate_terrain.py::test_job`: `wait:false` returns `job_id`; `get_job` reaches `succeeded`; `cancel_job` writes nothing;
  a sculpt during an `erode_existing` run fails with `terrain-changed`; refused in Play. → R33, R63
- **AC24. Preview.** `test_noise_graph.py::test_preview`: per-node ranges, `dead` and `folded` set on a fixture; `ops`
  changes the result without touching the file; a preset's output hash equals `generate_terrain`'s at the same size and
  seed; `images` limits hold; works while playing. → R64
- **AC25. Tool rules.** `test_generate_terrain.py::test_group`: hidden until `tool_groups` enables `terrain`.
  `tool_budget.py --check tools/mcp/budget.json`, `gen_tool_docs.py --check`, `test_tool_conventions.py`,
  `test_tool_coverage.py`, `test_docs.py`, `test_autosync.py` green. → R65, R66
- **AC26. Presets.** `NoisePresets.AllLoadAndValidate`, `.Character` (N14), `.EmbeddedMatchesTheSourceFolder`;
  `test_generate_terrain.py::test_every_preset_dry_runs`; screenshots of the eight terrain presets from an isolated editor. → R34, R35
- **AC27. Old outputs.** `TerrainNoiseCompat.MatchesTheOldHeaderTable` (captured first; the capture commit is the
  characterization run), `.ValueNodeExtendsTheOldValue`, `.FbmOfValueIsTheOldFractal`. Red control: 1013 changed to 1014 in a
  scratch tree fails. `grep -r TerrainNoise editor engine` finds nothing. `test_world.py` and `test_splines.py` unchanged and
  green. → R36
- **AC28. What stays.** `git diff` of `engine/core/math/Noise.h`, `engine/core/render/AtmosphereNoise.cpp`,
  `engine/core/math/noise/` and the particle sources is empty; `CurlNoise.*`, `AtmosphereNoise*` and the camera shake tests
  are green. → R37, N1
- **AC29. Flags and guards.** `tools/noise/check_kernels.py` as AC5; `python3 tools/noise/xcode_sync_noise.py --check` here (compiling the project needs a Mac and the
  Xcode workspace: not verifiable here); `tools/noise/cross_check.sh --red-contract` fails. Red-control output is in the evidence.
  → R38, R40
- **AC30. armv7.** If the image has qemu-user, `tools/noise/cross_check.sh` passes armeabi-v7a under `qemu-arm-static`, and a scratch patch
  forcing Neon on armv7 is run against the tiny-coordinate cases. If it goes red, the reason in N2 is verified; if not, keep Scalar, and report N2
  as inferred. Without qemu-user the row is reported unverified and N2 stays inferred. → R39
- **AC31. Cross-target, on this host.** `tools/build.sh unit-tests` (Linux x64, Scalar and SSE2, `NoiseDeterminism.*`, `NoiseGolden.*`);
  `WEB_PROJECT=noise tools/build.sh web`, `GATE=noise tools/build.sh web-gate`, and both again with `WASM_SIMD=0`;
  `tools/build.sh exec android tools/noise/cross_check.sh`, reporting each ABI as passed or `UNVERIFIED`. → R41
- **AC32. Cross-target, not verifiable here.** Windows, macOS and iOS: R72's commands. Android on a device: the matrix's command. The final
  report lists each as a recorded row of `docs/perf/noise.md` or says unverified. → R41, R72
- **AC33. Module.** `PlayerModules.*` includes `noise`; `tools/ci/check-presets.sh` covers the option error; a player
  built with `-DJASPERENGINE_NOISE=OFF` links; `tools/players/lib_size.py --modules` gives 0 for `noise`, and `nm` finds no
  `NoiseGraph`, `CompiledGraph` or `HeightfieldErosion` symbol, while `curlNoiseBatch` is still there (curl stays).
  Red control: the same check on the ON build finds them. → R42, R45
- **AC34. Detection.** `ModuleUsage.*`: each evidence kind for `noise` (Lua `Noise`, Lua `NoiseGraph`, a script property, a
  dynamic access, C++) makes it needed; an empty project and a project with only a `.noisegraph` and a baked terrain
  do not. `lua_api` lists the bindings under `noise`. → R43
- **AC35. Variants and export.** `test_noise_export.py`: a baked project exports with a `lite3d` runtime, no `.noisegraph`
  in the payload, and the same transcript as before. A `Noise` Lua project selects `core`, ships its graph and prints the
  expected hash when run. `test_player_cli.py` still matches. → R44, R45
- **AC36. Budgets.** `je-bench-noise --json` through `tools/perf/check_bench.py` on an idle host, with its entry in
  `bench-thresholds.json`: R46's x86-64 and Scalar columns, R47, R48, R49. Measurements in `docs/perf/noise.md`. The first run is
  the record; a budget lowered is a Deviation. → R46-R49
- **AC37. Web cost.** `?noise_bench=1` on the `noise` project reports R46's web columns (simd128 and scalar);
  `?noise_bench=1 --expect=noise_web_budgets=pass` is run on an idle host; on a busy one the run only reports. → R46
- **AC38. Editor cost.** `test_noise_graph_window.py::test_closed_costs_nothing`: after closing, `get_job_system_stats` shows
  no noise job and no thumbnail textures remain. `NoiseGraphDocument.PreviewsAreSkippedWhenClosed`. Main-thread budget measured with
  the profiler in the evidence (`manual`: a 64-node fixture under Xvfb). → R50
- **AC39. Size.** `tools/players/player_sizes.py build/players --modules` on the runtimes of `tools/build.sh players`: Linux
  and web on this host, arm64 from the Android build. → R51
- **AC40. Memory.** `TerrainGeneration.PeakBytesWithinBudget` at 1024², and `NoiseEval.NoAllocationOnceWarm` with the allocation
  counter of `engine/tests/core/bench`. → R52
- **AC41. Canvas.** `GraphCanvasCompat.NoPinsDrawsAsBefore`: a headless ImGui context, a fixed Animator-like graph, vertex
  positions compared with a table captured from the code before the change. `GraphCanvasPins.DragConnects`,
  `.DropOnEmptyRequestsSearch`, `.IncompatibleIsRefused`, `.WireHitTest`, `.ThumbnailAndFooterFit`.
  `test_animator_window.py` and `test_animator.py` unchanged and green. → R53, R54
- **AC42. Window logic.** `NoiseGraphDocument.GesturesAreOneUndoStep`, `.DragCommitsOnRelease`, `.IssuesMapToNodes`,
  `.StaleResultsAreDropped`, `.ExternalEditRefreshes`, `.BrokenFileRefusesEdits`, `.SearchFiltersByPinType`,
  `.DropAddsAndConnects`. → R55, R56, R57, R58, R59
- **AC43. Window, driven.** `test_noise_graph_window.py`: opens from the command catalog and a `.noisegraph`; Project window >
  Create > Noise Graph makes a graph with a GUID; `find_ui_item` finds `noisegraph.canvas`, Seed, Preview and Apply; an edit undoes; an error node exposes its message through `UIProbe`; the Console
  stays empty. Screenshots from an isolated editor (`tools/editor-shot.sh`): a graph with thumbnails and ranges, an error on a
  node. → R55, R56, R58, R62
- **AC44. Generate section.** `test_terrain_generate_ui.py`: Preview does not write; Apply is one undo step; Cancel writes
  nothing; settings persist across a reload; `StreamSchema` lint green. Screenshot. `TerrainGeneration.CreateDialogSeam` calls
  `TerrainPresets::list` and `start` with a fake dialog. → R60, R61, R70
- **AC45. Lua.** `test_noise_lua.py`: `Noise.*` and `NoiseGraph` in Play print hashes equal to the table; `sampleGridAsync` completes
  with `JE_JOB_WORKERS=0`; a bake-only graph returns `nil` and logs the node; `sample` on a grid-only graph errors. The docs' Lua blocks
  pass `tools/lua_api` doctest. On the web gate project, an async grid never exceeds the pump budget by more than 1 ms per frame.
  → R67, R68
- **AC46. Old projects.** `test_upgrade_from_main.py` green; `tools/ci/upgrade_from_main/check.sh` green if run; a project with neither
  key in `TerrainEditorSettings` loads with defaults (`StreamTerrainSettings.GenerateKeysDefault`). → R70
- **AC47. Docs.** `gen_tool_docs.py --check`; `python3 tools/check_agent_docs.py`; guide README row; `test_docs.py`. → R71
- **AC48. Self-check tool.** On this host `jasperengine-noise-golden --all-backends --report -` passes and `--self-test` exits 1; the line format is
  pinned by `NoiseGoldenTool.ReportFormat`; `GATE=noise tools/build.sh web-gate` expects `noise_selfcheck=pass`. → R72
- **AC49. Eval.** Cases in `tools/mcp/eval/cases.py`, run as T1 AC74 says (`-k noise_`, `-k terrain_generate`; lent accounts, else reported unverified).
  **Red:** a surface from before the tool. `terrain_generate_preset` (bad; "make mountains, seed 7": `generate_terrain` with `preset`, must not script
  `edit_terrain add_noise`), `noise_graph_edit_loop` (guard; "tune this graph": `edit_graph`, `preview_noise_graph`, then `generate_terrain dry_run`),
  `noise_graph_create` (guard; "start a new graph from alpine": `edit_graph {create}`, no `write_file`), `terrain_generate_error_next` (guard; a graph error's `next` is
  followed). → R65, R74
- **AC50. Asset type.** `AssetTypes.NoiseGraphValueIs28` pins the value, and `assetTypeFromName`'s bound covers it. → R13
- **AC51. Doriax merge.** Run by whichever lands second: `test_terrain_details.py` and `test_generate_terrain.py::test_layers_and_density` both green on the
  merged tree, with one `docs/mcp/guide/terrain.md`. → R75

**Red controls in one place** (each run recorded in the evidence folder, with the output):

| Claim | Red run |
|---|---|
| The table catches contraction | `cross_check.sh --red-contract` fails |
| Tile colouring is what gives worker-count bits | AC16's scratch patch |
| The old output is really preserved | AC27's 1014 patch |
| The compat table is the old code | Captured from the old header in a commit before deletion |
| Module OFF is really free | AC33's `nm` on an ON build finds the symbols |
| Canvas compat catches a change | A scratch one-pixel shift of a straight arrow fails `GraphCanvasCompat` |
| Dry run writes nothing | Scratch patch applying in dry run fails AC19 |
| armv7 scalar is needed | AC30, recorded as either outcome |

**What this host can and cannot verify** (8 cores, software GL, no KVM, no Windows, macOS or iOS; there is no CI, so nothing else checks a push):

| Result | Status here |
|---|---|
| All unit, MCP and sanitizer suites on Linux x86-64 | runs |
| Web gate, simd128 and scalar, in headless Chrome | runs |
| Android golden tool: x86 and x86_64 native; arm under qemu-user only if the image has it | see the matrix; a CPU emulator's FP is high fidelity for these operations but is not a phone. Android APK builds run; no emulator |
| Bench gates | runs, on an idle host only: run them with the queue empty and say so |
| Screenshots and UI tests | runs, software GL |
| Windows `cl`, macOS arm64, macOS x86_64, iOS | not verifiable here: R72's commands, unverified until recorded |
| Phone timings, `arm64 mobile` budgets, Android emulator gate | not verified anywhere in this programme; targets only, say so |

## 8. Dependencies & conflicts

**Depends on (landed):** S04 (asset table, GUIDs), S06 (jobs), S21 (`GraphCanvas`), S23 (graph model and tools), S27 (curve field,
for `curve` keys), S34 (modules), S20 (flags and gate precedents).

**Depends on (in this programme):** T1 for `TerrainOps`, `TerrainMapStore`, the Create Terrain dialog and the terrain panel's
structure. Programme landing order (T1 §8): T1's foundation first, then T1's rest, T2, T3, T5 and T6 in parallel, then T4
last. The library, compiler, erosion, presets, `preview_noise_graph` and `generate_terrain dry_run` need only that foundation. Real
runs need T1's `TerrainMapDeltaCmd`, and R61 its dialog, so those parts land after them.

**Seams T1 must honour**
- Absolute batch writes as in N9 under one undo step, with dirty rects and a height read for the snapshot.
- A place for the one-line `drawGenerateSection()` call; the Create dialog calling `TerrainPresets::list()` and
  `TerrainGeneration::start()` (R61).
- If T1 moves the Noise or Erode brush into `engine/core/terrain/`, they call `noise::compat` (always compiled, N13). They must
  not use the module's kernels, since players without the module would not link.
- Whichever of T1 or T2 lands second completes the removal of `TerrainNoise.h`.
- `paint_by_rules` in `edit_terrain` (T1) and `output_layer` (T2) must use the same normalization (R30): T1's
  `setLayerWeights` is the single encoder.
- The `terrain` group's `groups` entry in `budget.json` (D17) is T1 R74's (16,000 characters, 10 tools), added by T1's foundation, sized for all of T1-T5.

**Seams T6 must honour**
- It uses `NoiseGraphRuntime.h` only (R69, N15), one `EvalScratch` per worker, `evaluatePoints` or `evaluateGrid3D`, `rangeOver` for
  block clipping, and the named `output_value` (`sdf`).
- `JASPERENGINE_VOXEL` requires `JASPERENGINE_NOISE`; T6 adds the CMake check, its `ModuleUsage` evidence and any variant change.
- Voxel graphs use `world` space, `dimensions: 3d`, and no grid-only or bake-only node.

**Seams others use**
- T4 passes global texel starts in `GridDomain` for seamless tiles (R23), and writes reach its tiled store through T1's interface.
- T5 reads density through the existing density maps and T1's dirty rects. Its `scatter_by_rules` `noise_mask` (T5 R24) calls `compile`
  and `evaluateGrid` on the `output_value` named `mask`, or on the graph's only output. A graph with neither is its error to report. A
  run of it in a player needs the module, so any Lua path to it must carry the `noise` condition.
- T3's top-N layers sit behind `setLayerWeights`. T3's Base Map macro mode mentions T2 bakes: this spec bakes no colour map. A graph can
  produce the mask, and the colour bake is not in scope here.

**Merge hotspots**

| File | Others | Note |
|---|---|---|
| `engine/CMakeLists.txt`, `variants.json`, `PlayerModules.*`, `ModuleUsage.cpp:43`, `JasperPlayerVariants.cmake`, `Exporter.cpp:3589-3598,4611` | T4, T6 | Each adds a module: keep both lines |
| `tools/mcp/budget.json`, `McpTools.cpp` table, `docs/mcp/TOOLS.md` | T1, T3, T4, T5 | Regenerate after merging |
| `TerrainEditWindow.cpp` | T1, T3, T4, T5 | T2 touches two lines and adds one call |
| `AssetType.h`, `AssetTypes.cpp` | S43-S45, S50, T4, T5, T6 | Reserved values; move `assetTypeFromName`'s bound |
| `GraphCanvas.*` | S21 follow-ups | Additive; AC41 guards it |
| `docs/mcp/guide/terrain.md`, `tools/mcp/eval/cases.py` | T1, T3, T4, T5, T6, `agent-authoring` | Sections and cases only; no line in `McpInstructions.cpp` |
| `engine/core/terrain/` | T1 | T2 only adds `HeightfieldErosion.*` |

**Blocks:** T6 (noise), parts of T1's Create dialog (presets; T1 can ship with a flat option first).

**Doriax overlap (D21; none landed: whichever lands second merges onto the first).** `agent-authoring`: `docs/mcp/guide/terrain.md`, the guide restructure and
`edit_terrain_details`' layer indices, which `output_density` writes by. `render-correctness`, `draw-perf`, `ssr-probes`, `mesh-memory`: nothing in common with T2, whose
hooks stay out of `MeshSystem.cpp` and `RenderSystem.cpp`.
**Pixel art.** S50's "terrain" is tile autotiling and shares no name with T2; pixel art's `AssetType` values are 24-27.

## 9. Risks & defaults

- **qemu-user is not in the Android image** (`docker/android.Dockerfile`). Default: the ARM rows stay unverified. If the owner adds it, a static bionic binary may still not start under
  qemu-user (*inferred*; untested); the fallback is Ubuntu's `aarch64-linux-gnu` and `arm-linux-gnueabihf` cross compilers under qemu-user, which proves the ISA and flags but not
  NDK clang, recorded as a Deviation.
- **SSE2 emulates `floor` and the 32-bit multiply** (`SimdFloat4.h:13`), and this may miss R46. Default: budgets stand within the 30 %
  rule. An AVX2 path is a later spec that must pass the same table.
- **Another platform differs when a person runs R72.** Default: it is a kernel bug. Fix the kernel, which may change Linux bits too; before
  a kernel's first landing to `main` the table may be regenerated in the same commit, after that R14 applies.
- **Erosion can look wrong while being deterministic.** Hash tests cannot judge beauty. Default: AC17's behaviour tests plus a
  reviewed screenshot. Tile rounds (N6) may show a faint grid. Default: 4 rounds; if the screenshot shows it, more rounds with
  the same totals, adjusting goldens before landing.
- **T1's API differs from N9.** Default: T1's names win; `TerrainGeneration` is one adapter file, and this spec is not
  re-litigated.
- **A `terrain_height` snapshot of a 4096² map costs 67 MiB.** Accepted; only graphs that read it pay.
- **A preset's layer outputs on a terrain without those layers.** Default: `optional`, a warning, heights still apply.
- **Default seed.** 1, not random, unlike `set_terrain_maps`: agents get repeatable results and must ask for variety. The window's dice
  picks one and shows it.
- **Non-power-of-two frequencies drift across octaves in float.** Accepted: it is deterministic, and that is the requirement.
- **`terrain` space on a resized terrain.** A heightmap resolution change keeps the shape, since coordinates are normalized; a
  size change stretches it. Accepted: presets scale with the terrain, and `world` space exists for fixed scale.
- **A variant decision (N11).** A coordinator may prefer `noise` in `full` only. Changing it is one line in `variants.json`
  plus the AC35 expectation.
- **`random_noise` (R37).** T1 R19 makes it a seeded hash and records the changed output; it is not `TerrainNoise.h`, so
  `noise::compat` does not change for it.

## Deviations

Recorded by the implementer. Each names what changed and why; the spec text above is unchanged.

- **Output port ids of the erosion nodes (R9).** A node's ports share one namespace (`graph::portsOf`, `findPort`), so an input `height` and an output `height` cannot both exist. The input stays `height`; the output is `out`, named Height. `flow` and `sediment` are as written.
- **`terrain_height` is grid-only as well as bake-only (R9).** It reads a snapshot by texel, so it has no meaning at an arbitrary point. `isGridOnlyType` says so; `evaluatePoints` on it would otherwise have returned zeros silently.
- **The Value node extends `compat::value` on `[0, 65535)`, not `[0, 65536)` (N13).** The node's lattice wraps at the last cell, where the old code had no wrap. Wrapping keeps the field continuous across the period; the old value at that cell differs, and the tests cover the range that matches.
- **`EvalContext` and `CompileOptions` grew (N15).** Additive fields: the snapshot's extent and origin (a padded region must clamp to it), `uvPerUnit` (a region's UVs are the whole terrain's), `clock` (the seam of AC13), `backend`, `report`, and `previewNodes`, `maxHeight`, `forbidGridOnly`, `forbidBakeOnly` on the options. `evaluateGridOutputs` is new: outputs of one run share a stage, so an erosion runs once.
- **`metadata.noise.usage` and `metadata.noise.resolution` (R15, R26).** `validate_graph` has no call context, so a file states where it runs (`bake`, `runtime`, `points`) for `bake-only-node` and `grid-only-node`, and the resolution for `erosion-budget`, default 1024. `compile`'s options override both.
- **Layout of `evaluateGrid3D` (N15).** x fastest, then y, then z. The spec names no order; T6 R2 says only "x fastest".
- **Maps a run makes (T1 R24, R28).** A real run is T1's form of one undo step: the maps it needs and the terrain lacks (a replaced height map, blend maps, density maps) are made blank and kept by structural steps holding their final pixels, then one `TerrainMapDeltaCmd` skips them, grouped as "Generate Terrain (...)". T1's `editor/terrain/TerrainOpsUndo` makes those steps for `edit_terrain`; its two helpers are public now, and the blank map is sized by bytes per channel, as a 16-bit height map needs.
- **Create Terrain with a preset (R61).** One click is one undo step, "Create Terrain (Rolling Hills)": `CreateTerrainDialog::createGenerated` runs the preset through `TerrainGeneration::prepare`, `evaluate` and `apply` over a `RawMapStore` of the new height map on a job, then T1's `create()` makes the terrain with those heights (two optional arguments: the bytes and the step name). Generating after Create and grouping the two steps could not redo: T1's delta refuses while its terrain does not exist yet. `CreateTerrainDialog::show` moved from the Inspector's draw to the App's, since the Terrain window's own button opened a dialog nothing drew while the Inspector tab was behind it.
- **`sampleGrid`'s position (R68).** Samples are texel centres, `(round(x / step) + i + 0.5) * step`, so a script's grid has the bits a generation gives at those texels. `x` and `z` snap to the lattice.
- **The default `radius` is 2, not R24's 1 (coordinator's decision, overriding R24), and deposits go through the brush.** Measured with a scratch probe at 257 texels on 1000 m, `droplets_per_texel` 0.25, seeds and grounds varied, as a multiple of the fractal's own curvature (`terrain-evidence/T2/erosion-radius-probe-after.txt`):

  | `radius` | One-texel curvature, 5 / 8 octaves | Four-texel curvature, 5 / 8 octaves | M steps/s, one thread (busy host, with the in-bounds path; 5-octave ground) |
  |---|---|---|---|
  | 1 | 1.51-1.52 / 1.06-1.08 | 1.03 / 1.00 | 11.1 |
  | 2 | 1.04 / 0.95 | 0.97 / 0.93-0.94 | 9.0 |
  | 3 | 0.95-0.96 / 0.97 | 0.92-0.93 / 0.91 | 7.0 |
  | 4 | 0.99 / 0.98 | 0.91 / 0.91 | 4.7 |

  2 is the smallest radius that stops the roughening; 3 and 4 add little for much more cost. With deposits on the four cells under a droplet, as Beyer has them, `radius` 2 stood at 1.35 and 1 at 1.67 (1.95 at `droplets_per_texel` 1), which is why they share the brush. `radius` 1 stays selectable: it is the fastest and preserves fine detail, and it can roughen the ground (`docs/mcp/guide/noise-graphs.md`). `CarvesChannels` pins both scales at the defaults. Whether it looks right still needs AC17's reviewed screenshots, and R48's 8 M steps/s, met here at 9.0 on a busy host by a scratch probe, must still be gated by `je-bench-noise` on an idle one.
- **Hand-picked Simplex radius (R1).** 3D Simplex uses radius 0.5 rather than the paper's 0.6, which keeps the field continuous; the result is clamped to -1..1.
- **`NoisePresetData.h` (R41).** The test project's build has no hook to embed files, so the presets are a generated, committed header (`tools/noise/gen_test_project_presets.py`, `--check`).
- **The Android image gains cross compilers too (section 6, D22).** Besides `qemu-user-static`, `gcc-aarch64-linux-gnu` and `gcc-arm-linux-gnueabihf` are in the image, for section 9's fallback when a static bionic binary will not start under qemu-user.
- **`AssetType::NoiseGraph` is 33, not 28 (R13, AC50; coordinator's decisions).** Upstream's Sound Event took 28 before T2 landed,
  and S90's Procedural Material took 29 before the programme reached main; the value is stored in `assetmap.bin` and ours had never
  shipped, so the programme's types follow upstream's: TreeData 30, TerrainWorld 31, VoxelVolume 32, NoiseGraph 33.
  `AssetTypes.NoiseGraphValueIs33` pins it.
- **The canvas' ports are S90's, not a Pin API of T2's own (R53, N16).** Upstream's S90 gave GraphCanvas dataflow ports first
  (string port ids, `PortRef`, `canConnect` and `drawThumbnail` callbacks, a header cost). At the merge T2's parallel uint64 pins
  went; the Noise Graph window draws through S90's ports, and the canvas gained what it lacked: footer lines (the range readout, R56),
  the error's wrapped message and a port's error ring (R58). A node's time is its header cost; folded and unused are its first
  footer line; the zoom-0.5 rule for thumbnails (R56) is the window's `drawThumbnail`. Nodes are S90's 170-unit width with a 112-unit
  thumbnail slot. A `.noisegraph` opened through the Graph Editor's routes (`focus_window graph_editor`, a graph link) opens in the
  Noise Graph window (`GraphEditorHooks::kindWindows`).
- **"Connect FBm → Remap", not `graph::describe`'s text (R57).** `describe` names a connection's source by its port, and nearly every
  noise port is called Out ("Connect Out → Remap"). The window names it by the two nodes' display names instead.
- **Previews run on `Lane::Normal`, not R28's Background + Heavy (coordinator's decision).** The reason is the gate's arithmetic,
  read from the code, not measured here: a desktop editor has cores - 1 workers (ThreadUtil.cpp), and BackgroundGate admits
  max(1, workers - 2) Background bodies and Heavy at most max(1, that - 2). On a 4-core machine that is one slot, which the editor's
  start-up shader compiles (Heavy, ShaderBuilder.cpp) hold for their whole run, so a Preview a person clicked would queue behind every
  one of them; S06 has no lane for user-initiated work. This host's containers report 8 cores (7 workers, 5 Background slots, 3 Heavy),
  so the stall was not reproduced here, and the Generate section's hung first Preview seen during testing had another cause. The
  Generate section's Preview (capped at 513 per side, `kInteractiveResolution`), `preview_noise_graph` (at most 512) and the window's
  thumbnails run on Normal, as the search palette's interactive job runs on High. A newer preview cancels the older for the same
  document, graph or terrain: `NoiseGraphDocument.ABurstOfPreviewsRunsOnlyTheNewest`, `test_noise_graph.py::test_preview`; the
  section runs one at a time, its controls disabled while it runs. `generate_terrain`'s full-resolution dry runs, Apply and real runs
  stay Background + Heavy. None of this is runtime code: the Lua `sampleGridAsync` stays on Background, inside a web player's pump.

