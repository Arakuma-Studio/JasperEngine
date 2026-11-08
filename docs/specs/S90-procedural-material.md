# S90 — Procedural Material: node graphs baked in the editor to textures and a material

**Wave / Depends on.** Authoring wave (spec-first). Depends on S23's graph model
(`engine/core/graph`), S04 (GUIDs), S05 (hot reload), S14 (texture cooks) and S24 (undo), all on
`main`, and on **BF23**, which fixes the graph tools first. Independent of S91. Owner area:
`editor/procmat/` (new), `editor/window/widget/GraphCanvas.*`,
`editor/window/ProceduralMaterialWindow.*` (new), `editor/mcp/tools/ProceduralMaterialTools.cpp`
(new), `docs/mcp/guide/procedural-materials.md` (new).

**Basis of this spec.** The current state below comes from reading the code at `b4f7816ea`. M0 has
run: the numbers in R6, R12, R23 to R25, AC8 and Deviations are measured where they say so, and
defaults where they say that. Effort figures are estimates.

## 1. Goal

A user or an agent builds a material from a graph of procedural nodes, sees every step live, and
bakes it to ordinary PNGs and an ordinary `.material`. A game never knows a graph existed: nothing
new ships, and a baked material runs on all five backends and six targets through S14.

The node library is ported as data from Material Maker (MIT). Its Godot host, its compute pipeline
and its export system are not. Evaluation runs on the best device available, in a fixed order: the
GPU, then a software renderer, then the CPU. All three run the same generated shader, so no node is
written twice.

A human and an agent see the same evidence: one implementation per contact sheet and per check.

## 2. Scope

**In scope**

- `.procmat` asset, a new S23 graph kind, with a version and migrations.
- The evaluation pipeline with its three device tiers, including a SPIR-V interpreter for the CPU
  tier.
- A bake that writes 8-bit PNGs and a `.material` as one undo step, with staleness tracking.
- The ported node library, behind a licence deny-list.
- `GraphCanvas` ports, wires and thumbnails; the Procedural Material window.
- MCP tools, a `check` CLI flag, a bake CLI subcommand, docs.

**Out of scope**

- **Live generated shaders (a Shader Graph).** A later spec. The name "Shader Graph" stays reserved
  for it, which is why this feature is not called that. `S11:40` excludes OpenEngine's shader graph,
  and this spec does not change that: a bake adds no shader to any game.
- **16-bit or float output.** 8-bit only in this spec (decided by the owner).
- **Running a graph in a game.** No runtime evaluator, no Lua API: this is not runtime behaviour.
- `.ptex` import, 3D painting, Mesh Map nodes, the website and sharing, and user-written GLSL nodes.
- Vendoring a software rasterizer (§9, risk 1).

**Do not copy from Material Maker**

- `pipeline/*.gd`, `buffer_compute.tres`, `iterate_buffer_compute.tres`: compute code.
- `custom` export scripts, `process_option_*`, the `$(expr:...)` evaluator, `website:` loading.
  These execute or fetch from data files.
- The deny-listed nodes in R18. Licences live in comments inside data files, so a folder-level MIT
  check would pass and be wrong.
- Non-code assets: HDRIs, meshes, fonts, splash screens.

The do-not-copy list in `docs/specs/README.md` is respected: `GraphCanvas` is our own, and no
OpenEngine canvas, toolkit or `SurfaceOutput` adapter is taken. `S21` D14 rejected imnodes and
imgui-node-editor; this spec keeps that.

## 3. Current state

- **Graph model.** `engine/core/graph` has typed ports, JSON, validation, invertible ops and
  migrations. MCP has `list_graph_kinds` to `edit_graph`
  (`editor/mcp/tools/GraphTools.cpp:230-272`). The only registered kind is a test one
  (`GraphTools.cpp:36`). `S23:50-51` names shader and VFX graphs as intended uses.
- **Canvas.** `editor/window/widget/GraphCanvas.{h,cpp}` (448 lines), used only by the Animator. It
  has pan, zoom, grid, box select and straight arrows. It has no ports, no curved wires and no node
  images.
- **Texture asset path.** Texture Synthesis and Sprite Normals already write a PNG with a `jasper.*`
  recipe chunk as one undo step, with a session class, window, MCP tool and CLI. The chunks are
  stripped at export (`Exporter.cpp:2882-2885`). `SynthesisSession.h`, `VatBakeSession.h`,
  `SpriteNormalsWatcher.h` and `Project::writeMaterialFile` are the patterns to follow.
- **Offscreen passes.** There is no helper, but the parts exist: a vertexless fullscreen pass
  (`RenderSystem.cpp:2941-3060`) and a synchronous read back (`GraphicUtils`, RGBA8 or R8 only).
- **Editor backends.** GL 4.1 core, Vulkan or Metal only (`CMakeLists.txt:88-98`). D3D11 and GLES3
  are never live in the editor, so a bake never needs them.
- **No GPU today.** With no GL 4.1 context the editor prints an error and exits
  (`BackendLinux.cpp:2633`, `BackendWin.cpp:753`). Vulkan prefers a non-CPU device and accepts a CPU
  one (`RendererVulkan.cpp:203`). `--headless` on Linux runs on Xvfb with `LIBGL_ALWAYS_SOFTWARE=1`
  (`McpLaunch.cpp:285-299`). Windows and macOS have no software path. `SystemRender::getDeviceName`
  names the device.
- **Shader compiler.** glslang and SPIRV-Cross run in-process
  (`ShaderVariantCompiler.cpp:1293-1316`). `compile_to_spirv` returns `std::vector<uint32_t>`. The
  `shaders` CLI subcommand runs before the backend starts (`editor/main.cpp:47-53`), so compiling
  needs no window.
- **Agent traps in the graph tools.** Listed and fixed in BF23 (F5 to F12).

## 4. Reference: Material Maker at `7d6daae`

Clone it to a scratch directory and treat it as untrusted: read it, never run it.

- **Take as data:** `addons/material_maker/nodes/*.mmg` (GLSL and metadata),
  `shader_functions.tres`, `io_types.mmt`, and `material_maker/library/base.json` with
  `aliases.json` for categories and search.
- **Use as a behaviour reference only:** `engine/nodes/gen_shader.gd` (macro expansion),
  `gen_base.gd:362-390` (uniform-only edits and the 0.2 s rebuild debounce), `gen_buffer.gd`,
  `gen_iterate_buffer.gd`.
- **Library shape, by count of the JSON:** 328 entries: 285 single pass, 31 Buffer, 6 Iterate
  Buffer, 6 host-dependent. None of the 399 `.mmg` files contains a compute construct, and none uses
  `dFdx`, `dFdy` or `fwidth`. One uses `discard`. Only the host's compute pipeline needs compute.
- **What it gets wrong, on purpose not copied:** no format version; nodes bound by name; a
  text-level code generator; a manual Buffer workflow; export scripts that evaluate data; float
  parameters inlined into GLSL unchecked; a CLI that exits 0 on failure.

Its random functions are float hashes (`fract(cos(mod(dot(x, ...), 3.14)) * 43758.5)`,
`shader_functions.tres:12`). These vary across devices, which R10 must fix.

## 5. Requirements

### Functional: the asset

- **R1.** `.procmat` is deterministic S23 graph JSON: kind id `procedural_material`, `kindVersion`
  1, `AssetType::ProceduralMaterial` (the next free value at landing; 24-27 belong to the pixel-art program) with its GUID in the file. It is a new file type, so old
  projects are unaffected.
- **R2.** Nodes are referenced by a stable type id. A change to a node's output gives it a new
  `typeVersion` with a kind migration that keeps the old behaviour selectable. A graph never changes
  look silently because the library was updated.
- **R3.** A file is data only. No field is executed or fetched. Parameter values are validated
  against their declared type and range before reaching GLSL, and strings never do.
- **R4.** `.procmat` files never ship: add them to the export skip list. Test it, because
  `copyAssets` copies whatever it does not skip (`Exporter.cpp:2886`).

### Functional: evaluation

- **R5. Code generation.** A graph becomes one fragment shader per pass, not per node. A pass is the
  upstream subgraph inlined, cut at a Buffer. Use a C++ expander with stable graph-local names and
  Material Maker's macro semantics, resolving `global` functions by id rather than by exact text.
- **R6. Automatic caching.** The generator inserts a Buffer when a pass would exceed 16 textures,
  64 KB of generated GLSL, or 200 vec4s of uniforms, or when one output is sampled repeatedly. Users
  never need to place Buffers to manage performance. The limits are M0's: a 240-node pass (86 KB,
  68,000 SPIR-V words) compiled and ran on every path, so size is not a hard wall. 64 KB is where
  SPIRV-Cross reaches about 150 ms, since it grows faster than the source (22 ms at 36 KB, 358 ms at
  86 KB). GL 4.1 guarantees only 256 fragment vec4s, and M0 measured 4.7 floats per node.
- **R7. Live edits.** Float, colour and same-size ramp edits update a uniform block with no rebuild.
  Structural, enum and size edits rebuild after a 0.2 s debounce, and the previous image stays
  visible marked as out of date until the new one is ready.
- **R8. Device tiers.** A single interface, `ProceduralEvaluator`, with three implementations tried
  in this order:

  | Tier | Name in UI and results | Used when |
  |---|---|---|
  | 1 | `gpu` | The editor's context is on hardware |
  | 2 | `software` | The editor's context is on a software renderer (llvmpipe, lavapipe, or any other detected) |
  | 3 | `cpu` | No context exists, or a pass failed on the current device |

  Detection uses the Vulkan `deviceType`, or a list of known software names against `GL_RENDERER`.
  An unrecognised name counts as `gpu`.
- **R9. Falling back.** If a pass fails to compile or read back on tier 1 or 2, evaluation of that
  graph continues on tier 3 and reports a warning naming the failed pass, device and compiler log.
  It never fails silently and never leaves stale pixels in the output.
- **R10. The same noise on every tier.** The ported `rand` functions use an integer hash of
  coordinates quantized to 1/65536, with no `sin`, `cos` or `fract` of a large float. A cross-tier
  difference must be rounding, never a different pattern. Any node with its own float hash (four
  today) is changed the same way or dropped.
- **R11. The CPU tier is a SPIR-V interpreter.** `compile_to_spirv` turns the same generated source
  into SPIR-V, and a new `SpirvInterpreter` (`editor/procmat/`) runs it per pixel on the S06 job
  system's background lane. It covers the arithmetic, control flow and `GLSL.std.450` that the
  library uses, with no derivatives. Samplers do bilinear, repeat and box-filtered mips, matching
  the GPU tiers. An unsupported instruction is a reported error naming the node, found at load
  rather than per pixel.
- **R12. Intermediate precision.** Buffers are RGBA16F on tiers 1 and 2 (renderable on all editor
  backends, `Render.h:387-392`). Tier 3 rounds to half precision when storing a Buffer, so
  differences do not accumulate across passes. Final outputs are RGBA8 through a quantize pass,
  because read back is RGBA8 only. Every pass ends in `jp_finite`, which writes 0 for NaN using
  `isnan`, and the quantize pass clamps in the shader. Each tier otherwise gives NaN and infinity its own 8-bit value
  (M0: 127/255 apart on real nodes).
- **R13. Same inputs, same output.** On one device and tier, the same graph, seed and size give the
  same bytes. Across tiers, the difference stays within the tolerance in AC8. No claim of byte
  equality across devices is made anywhere, in the UI or in tool output.

### Functional: bake

- **R14. Outputs.** Base Color (sRGB), Normal, ORM (occlusion, roughness, metallic in one PNG), and,
  when connected, Emission and Height. 8-bit RGBA or R8. Square, power of two, 64 to 8192. Normals
  are computed from the float height before quantizing, in the engine's convention
  (`docs/textures.md`).
- **R15. Writing.** One undo step (`writeOutputsUndoable`) writes the PNGs and the `.material`:
  - Import settings are written explicitly in `.meta` rather than inferred, since an ORM in both
    slots meets the occlusion inference rule (`docs/textures.md:66`).
  - Each PNG holds a `jasper.procmat` recipe chunk: graph GUID, input hash, seed, size, tier and
    device.
  - A re-bake updates only the slots and factors it owns. Other edits to the `.material` survive.

  Default output: a sibling folder named after the graph. Baked PNGs are committed like any texture.
- **R16. Staleness.** The hash covers the graph, resolved node sources, and input image GUIDs and
  content. It excludes tier and device, so a CPU bake is not stale against a GPU bake. A stale bake
  shows a badge and an Inspector line, and `check` reports it. Nothing re-bakes automatically.

### Functional: node library

- **R17. Import tool.** `tools/procmat/import_mm_nodes.py` runs once, by a developer, on a scratch
  clone, with `python3 -I`. It writes our own descriptors to `editor/procmat/nodes/`, tolerating the
  trailing commas in `smooth_curvature2.mmg` and `io_types.mmt`. It fails on any attribution comment
  it does not recognise.
- **R18. Deny-list, never imported as they stand.** `skewed_bricks` and `skewed_uneven_bricks` (CC
  BY-SA: rewrite `get_line_intersection` from scratch and they may return), `shard_fbm`,
  `voronoi_triangle`, `splines`, `hbao`, `sdf3d_fbm`, `sdf3d_fbm2` (no stated licence).
  `pentagram.ptex` must not be a test file.
- **R19. Notices.** The 26 nodes under their own MIT or BSD-2 notices keep them in the descriptor
  and get entries in `NOTICE.md`, as does the library as a whole.
- **R20. What ships.** Single-pass nodes and Buffer and Iterate Buffer graphs, excluding the 3D
  category, that pass M0's compile sweep and a per-node image test on all tiers. A node that fails
  is left out and listed in the Deviations section, not patched around.

### Platform and backends

- **R21.** Evaluation runs only in the editor, on its GL, Vulkan or Metal context and on tier 3. No
  engine or runtime code changes. No compute, SSBO, bindless, indirect draw or 64-bit atomic is
  used, even in the editor, which keeps a future runtime path open.
- **R22.** A player build's size and frame cost are unchanged. The evaluator and interpreter are
  editor-only targets.

### Performance

Measured by M0 on an AMD EPYC Genoa host through `slot` (4 CPUs, shared with other builds), llvmpipe
with `LP_NUM_THREADS=4`. No GPU was available, so every tier 1 number stays a default.

- **R23.** Slider drag on tier 1: the frame cost stays under 16 ms on the reference GPU, for a
  30-node graph with 256² previews. **Default, not measured.** For scale, the same pass on llvmpipe
  draws in 14 ms, and a float edit needs no compile (R7).
- **R24.** Thumbnails: at most one node re-rendered per frame, one shared quad scene, an LRU of 128²
  framebuffers. No `MaterialRender` per node: the sokol pools are 2048.
  - Tier 2: thumbnails render for the selected node and the outputs only. Measured: a first draw of
    a 128² thumbnail is 35 ms median and 165 ms at p95, mostly llvmpipe's JIT, so one per frame
    would miss 16 ms and the restriction stands.
  - Tier 3: only on selection, as a cancellable job. Measured at 128² on 4 cores: 3 ms median,
    100 ms at p90, 2.1 s worst.
- **R25.** A 1024² bake of a 30-node graph: under 2 s on tier 1 (**default**), under 5 s on tier 2,
  under 5 min on tier 3 on 4 cores. Over 1 s is a job with progress and cancel; a cancelled bake
  writes nothing. Measured for 1024²: tier 2 is 0.19 s for a 30-node pass, JIT included (0.15 s after
  it), and 0.3 to 0.6 s for the heaviest real graphs (wood, marble, dirt, every output). Tier 3 is
  30 s for that pass and 37 to 42 s for those graphs. At 2048² the 30-node pass takes 1.4 s on tier 2
  and 120 s on tier 3. The exception is `japanese_glyphs2`, whose first draw on llvmpipe is 71 to 78 s.

### Editor UX (human)

Window name **Procedural Material**. Double-clicking a `.procmat` opens it. Create it from **Assets
> Create > Procedural Material**. Add the row to `docs/unity-glossary.md`.

- **R26. Graph interaction.**
  - Ports and curved wires, coloured by type.
  - Dropping a wire on empty canvas opens search filtered to compatible nodes, auto-connects, and
    places the new port under the cursor.
  - Ranked search with aliases, and enum values as entries (`SearchScorer`).
  - A thumbnail and a cost in ms on every node. Zooming out fades controls and keeps the thumbnails.
  - Parameters are edited in the Inspector through `colorField`, `curveField` and `gradientField`,
    never in modal dialogs. Procedural ramps can have up to 32 keys, while the particle limit of 8
    stays.
  - Esc cancels any drag and adds no undo step. No overloaded mouse chords.
  - Copy and paste nodes and values as JSON text.
- **R27. Preview pane.**
  - Follows the selection, with Pin output and two slots. Shows a lit ball (`MaterialRender`), a 2×2
    tile and one image per output.
  - A Problems strip with the same rows as the agent's checks. Clicking selects the node, and Fix
    applies `fix_hint` when it is a single edit.
  - Seed field with lock and reroll, plus Variations: a grid of 9 seeds, click to adopt.
  - The tier and device always show. On tiers 2 and 3 the Bake button shows an estimated time, and
    the window says why: "No GPU available, baking on the CPU."
  - Exposed Properties and Presets for values a user changes often.

### Agent UX (MCP, CLI)

BF23 delivers the graph tool fixes this feature relies on: strict parameters and did-you-mean (F5,
F9), new ids from `edit_graph` (F6), defaults with a supplied id (F7), edge ids on load (F8),
tracked and text graph extensions (F11), and the after-change hook (F12). On top of them:

- **R28.** The `procedural_material` kind sets `strictParams`.
- **R29.** `.procmat` is the kind's `fileExtension`, so a direct file edit reports problems through
  `external_changes`.
- **R30.** Nodes with no position get an automatic layout when the graph opens, plus a `layout` op,
  so a human opening an agent's graph never sees a pile.
- **R31.** Symbolic ids such as `bricks` stay legal, with a test pinning it.

New surface, none of it in core:

- **R32. Groups.** New group `graphs` holds the five graph tools, moved from `scripting`. Hidden
  tools stay callable, so no client breaks. New group `materials` holds the two tools below.
- **R33. Catalog.** `list_graph_node_types` gains `category`, `search`, `ids` and `detail`. Its
  default reply is one signature line per node: `stones(scale=8[1..64] variation=.5[0..1] seed:int)
  -> height:gray mask:gray`. An unfiltered large kind returns categories and counts only.
- **R34. `preview_procedural_material`** (read-only unless `save_path`). Takes `path` (`.procmat`,
  `.material` or an image), or `entity` and `submesh`; `sheet`, `size`, `seed`, `checks`, `fail_on`,
  `reference`, `save_path`, `wait`, `timeout_seconds`. Returns a contact sheet, metrics and checks.
  `sheet: []` means metrics only. It always returns `device: {tier, name}`.
- **R35. `bake_procedural_material`.** Takes `path`, `output`, `size`, `seed`, `outputs`, `fail_on`,
  `overwrite`, `wait`, `timeout_seconds`. Refuses while the graph has errors, and returns the undo
  id and the device.
- **R36. Metrics and checks.** Use S35's row shape (`id, kind, status, measured, threshold,
  threshold_source, limited_by, at[], fix_hint, method`) and its anti-gaming rules: changed
  thresholds are reported, and thresholds are project settings rather than tool parameters. Blocks:
  `tileability` (fail above seam 1.5, as in `synthesize_texture`), `repetition`, `histogram`,
  `base_color`, `normal`, `roughness`, `metallic`, `cost`. New check `height_8bit`: warns when a
  height output drives parallax, because steps will show.
- **R37. Reference comparison** is advisory and has no status: `palette`, `spectrum`, `edges`, from
  a local file the agent names.
- **R38. Never a stale sheet.** A sheet waits on `MaterialRender::isPending`, and past the wait
  bound it fails with an error. The thumbnail path's give-up-and-save (`ProjectWindow.cpp:604-617`)
  must not be reused.
- **R39. CLI.**
  - `jasperengine-editor check --materials` reports stale bakes and graph problems, in compiler
    style.
  - `jasperengine-editor procedural-material bake <file>` first tries a one-shot editor (tier 1, or
    tier 2 through `--headless` on Linux) and, if none can start, evaluates in-process on tier 3
    with no window. It exits non-zero on any failure and on unknown flags.

### Data and compatibility

- **R40.** `kindVersion` migrations go through `GraphRegistry` from the first version. Unknown keys
  are preserved at file level, as S23 specifies.
- **R41.** Recipes and metadata use existing fields. No change to `.material` format.

## 6. Design constraints and decisions

- **D1. Bake first.** Rejected for now: a runtime generated shader, which needs a surface hook in
  `pbr.glsl`, a depth fork for displacement, and a graph evaluated per pixel on phones. Rejected
  outright: a compiler in games, which costs 1.42 MB and draws nothing until compiled. The emitter
  in R5 is written so a later Shader Graph can reuse it.
- **D2. Interpreter, not a second node set.** Writing each node in C++ would double the library and
  drift. A software rasterizer shipped everywhere is large and still needs a context. Running the
  same SPIR-V keeps one definition per node. The price is speed, which only tier 3 pays.
- **D3. Interpreting SPIR-V gives us a reference.** There is no CI, and only Linux builds on our hosts. The CPU tier needs no context, so the evaluator and every node test can run anywhere the unit tests run, including a Windows or macOS machine with `--gtest_filter=-*Gpu*`.
- **D4. A tier is a property of the process.** A running editor cannot change its renderer, so a
  hardware failure in a live editor drops to tier 3, not tier 2. Tier 2 is reached by starting on a
  software renderer, which R39 does on Linux.
- **D5. A graph is authored from one text format.** No DSL: the JSON stays, and BF23 with R30 removes what makes writing it by hand painful. No per-node tools.
- **D6. Two tools rather than one with a flag,** because annotations must be accurate: a read-only
  preview works in read-only HTTP mode (`conventions.md:84-85`).
- **D7. Reuse over new widgets:** `GraphCanvas` is extended, `AnimatorAssetDocument` is the model
  for `GraphDocument`, and the Animator's Problems strip is the pattern for R27.

```cpp
namespace jasperengine::editor::procmat {
    enum class EvaluatorTier : uint8_t { Gpu, Software, Cpu };

    class ProceduralEvaluator {
    public:
        virtual ~ProceduralEvaluator() = default;
        virtual EvaluatorTier tier() const = 0;
        virtual std::string deviceName() const = 0;
        virtual bool prepare(const PassPlan& plan, std::string& error) = 0;
        virtual void setUniforms(uint32_t pass, const UniformBlock& block) = 0;
        // false when more work remains; a slice stays within budgetMs
        virtual bool advance(double budgetMs, EvaluationResult& result) = 0;
        virtual void cancel() = 0;
    };
}
```

**Budget** (characters, drafted in the design study; `tool_budget.py` replaces them):
`preview_procedural_material` about 1,350, `bake_procedural_material` about 930, graph tool
extensions +514. Core +0. Guide: at most one line in its "Other areas" index, under the 8,192-byte limit, with a case in `tools/mcp/eval/cases.py`; detail lives in `docs/mcp/guide/procedural-materials.md`.

## 7. Acceptance criteria

| AC | Check | Verified by |
|---|---|---|
| AC1 | A `.procmat` round-trips byte-identically and migrates from a synthetic older `kindVersion` | `GraphProcMatTest` |
| AC2 | A `.procmat` under `assets/` is absent from an exported game, and baked PNGs have no `jasper.*` chunk there | MCP export test, with red control |
| AC3 | A float edit causes no rebuild; an enum edit causes exactly one after the debounce | Unit test counting `prepare` calls |
| AC4 | A hardware device reports `gpu`; llvmpipe and lavapipe report `software`; an injected compile failure falls back to `cpu` with a warning naming the pass | Editor test plus MCP test on `--headless` and the RTX lane |
| AC5 | The bake CLI succeeds on tier 3 with no `DISPLAY` and no Xvfb on `PATH`, and exits non-zero on a bad graph and on an unknown flag | MCP harness shell test |
| AC6 | Same graph, seed, size and tier: identical bytes across two runs | Unit test per tier |
| AC7 | `SpirvInterpreter` matches an independent Python oracle on an instruction test set | `SpirvInterpreterTest` |
| AC8 | Tiers differ by at most 2/255 mean and 4/255 at p99, per output, per node in the shipped library. Measured between llvmpipe and the interpreter: 1,373 of 1,379 shaders pass, with a worst of 0.31 mean and 2 at p99 among them. No hardware GPU was compared, so the limit stays the default | `ProcMatCrossTierTest`, with a red control using the old float hash |
| AC9 | A bake is one undo step, and undo removes every file. Re-baking keeps a hand-edited field in the `.material` | `test_procedural_material.py` |
| AC10 | Editing an input image marks the bake stale; a tier change alone does not | Unit test |
| AC11 | The import tool fails on an unrecognised attribution comment, and no deny-listed node appears in the output | `test_import_mm_nodes.py` |
| AC12 | A misspelled parameter returns an error with did-you-mean, new ids come back from `edit_graph`, and a positionless graph opens laid out | `test_graph.py` |
| AC13 | `materials` and `graphs` are hidden until `tool_groups` enables them, tested with `ed.tools(groups=None)` | `test_procedural_material.py` |
| AC14 | Past its wait bound a preview returns an error, never an image of the previous material | MCP test with a forced slow compile |
| AC15 | The window: wire-drop search, Esc cancel, Fix, Variations and the tier line, with real input | `test_procedural_material_window.py` (`ui_probe`), and screenshots from an isolated editor |
| AC16 | A baked material renders on glcore and Vulkan goldens, and goes through S14 cooks for web and Android | Golden test, `tools/smoke-test.sh` |
| AC17 | R23 to R25 hold, or the measured numbers replace them here | Bench recorded in `docs/perf/procedural-material.md` |
| AC18 | Player size and frame cost are unchanged | S34 size check |

## 8. Dependencies and conflicts

- **Merge hotspots:** `AssetType.h`, `AssetTypes.cpp`, `ProjectWindow.cpp`, `App.cpp`,
  `McpToolGroups.cpp`, `tools/mcp/budget.json`, the agent guide.
- **S35** shares the check row shape and the guide bytes. Put the row in `editor/util/QaChecks` and
  let the second to land adopt it.
- **S91** shares only `QaChecks`, and does not touch `GraphCanvas`. Either can go first.
- **S11's** exclusion of a shader graph stands. **S23's** group table and S31's recorded groups
  change with R32.

## 9. Risks and defaults

1. **No software tier on Windows or macOS.** Default: detect one if the system provides it, and go
   straight to tier 3 otherwise. Shipping Mesa or SwiftShader adds tens of MB for a rare case, so it
   stays out unless asked for.
2. **A hardware failure in a live editor skips tier 2** (D4). Default: accept it. Spawning a child
   editor is possible on Linux only.
3. **Tier 3 could be too slow.** M0 measured it: a 1024² bake is 30 to 42 s on 4 cores. It is a
   fallback, and the UI says so.
4. **The library may compile less widely than expected.** M0: all 250 imported nodes compile on all
   six targets, after the import-level fixes in Deviations. Only GLSL 410 went through a real driver.
   MSL, HLSL and GLES 3.00 were produced by SPIRV-Cross and never compiled by their own compilers.
5. **Tile checks can be gamed,** and a metric cannot judge taste. Default: S35's rules plus
   `look_at`, handing the call to a human.
6. **8-bit height shows steps under parallax.** Default: `height_8bit` warns. 16-bit needs per-API
   read back and is a later change. The `RED16` format cannot be sampled on GLES3 or WebGL2 anyway
   (`S11:718-720`).
7. **Shader size blowup from repeated sampling.** R6 cuts passes, with M0's limits.

## Milestones

Effort in person-weeks, estimates at ±50%.

- **M0 (about 1).** Spike. Compile every single-pass node through `shadercompiler` for GLSL 410,
  SPIR-V and MSL, and count failures. Prototype the interpreter on three nodes and measure. Measure
  cold compile and 2048² bake time. Replace the defaults in R23 to R25 and AC8. **Decides whether
  the plan stands.**
- **M1 (under 1).** BF23 merged. Group move R32, R28 to R31.
- **M2 (4–6).** `GraphCanvas` ports, wires and thumbnails, `GraphDocument`, `GraphEditorWindow`,
  node search and Inspector (R26).
- **M3 (3–4).** Kind, generator, GPU evaluator, node import, R1 to R7, R10, R12, R17 to R20.
- **M4 (3–5).** Bake, staleness, preview tools, metrics, Problems strip, R14 to R16, R27, R33 to
  R38.
- **M5 (3–5).** `SpirvInterpreter`, tier selection and fallback, bake CLI, R8, R9, R11, R39, AC4 to
  AC8.
- **M6 (about 1).** Budgets, docs, guide, final gate. AC17, AC18.

## Deviations

### M0 result: the plan holds

All figures are verified by running `tools/procmat/sweep` (commands below) unless marked otherwise.

- **Compile sweep.** 250 of 250 imported nodes pass: 216 single-pass and 34 with Buffers, expanded
  into 1,379 shaders (every output and pass, unconnected and wired, each enum value and boolean,
  generic size 2). Each shader went to SPIR-V, GLSL 410, GLSL 300 es, MSL macOS, MSL iOS and HLSL 5.0
  with no failure. The first run failed 87 shaders on 16 nodes, and every cause was at import or
  expansion (below). No node is left out for failing to compile.
- **Cold compile.** glslang 2.4 ms median, 4.4 ms at p95, 38 ms worst; SPIRV-Cross 1.0 to 1.8 ms
  median by language, 5 ms at p95, 81 ms worst. The first compile in a process costs about 140 ms
  more.
- **Interpreter.** On 4 cores, in seconds per Mpixel: `perlin` 1.16, `voronoi2` 4.55,
  `fast_blur_shader` 0.85, `fbm4` 1.11, `bricks3` 0.10. Across the library's default shaders: 0.18
  median, 6.3 at p90, 45 at p99, 130 worst (`fill2`'s loop pass). One thread costs about 3.8 times as
  much. llvmpipe on the same nodes: 0.009, 0.008, 0.074, 0.021, 0.003, so tier 3 is 11 to 570 times
  slower than tier 2 on the same host.
- **Instruction set.** The library uses 96 SPIR-V opcodes, of which 61 execute and 35 declare, and
  38 `GLSL.std.450` instructions. The prototype covers all of them: 1,379 of 1,379 shaders load and
  run, in `editor/procmat/SpirvInterpreter.*`. A later milestone wires it to jobs and tiers.
- **Cross tier.** Interpreter against llvmpipe at 128², all 1,379 shaders: 1,358 are identical at
  p99, 14 differ by 1 step and 1 by 2 steps. Six exceed AC8, on 5 nodes:
  - Undefined arithmetic on an unconnected default: `height_to_angle` (`atan(0, 0)`) and `fill_to_uv2`
    (division by a zero box).
  - A near tie: `bricks` with `pattern=1`, output 6, takes `sign()` of a value that is zero up to
    rounding.
  - `classic_kuwahara` with `buffer=true` (0.46 mean, 14 at p99) and `auto_tones`' loop pass on two
    variants (2.17 mean, 37 at p99). Their causes were not traced; `auto_tones` was fed a probe image
    that its loop does not expect (inferred).

  R20's per-node image test decides these. **Red control for R10:** the old float `rand` on the same
  run puts 48 of 427 default shaders over AC8 (`perlin` 50 mean, 138 at p99; `noise_white` 84 and
  229), against 0 for both on the integer hash, excluding the cases above.
- **What was not measured.**
  - No hardware GPU and no lavapipe. A GPU can round more differently than llvmpipe does, so AC8 and
    all of tier 1 stay defaults until the RTX lane runs them.
  - AC8's run is at 128² for all shaders, and at 256² for default shaders only, where the same three
    of 427 are over (`auto_tones`' loop, `fill_to_uv2`, `height_to_angle`). Every pass samples
    the same 256² probe image, so no Buffer feeds another pass: R12's half rounding and any
    accumulation across passes are unmeasured until M5 runs real graphs.
  - These timings ran with the host at a load of 10 to 14. A rerun after the review fixes, at higher
    load, gave `perlin` 1.78, `voronoi2` 7.01, `fast_blur_shader` 1.24 s per Mpixel, with llvmpipe also
    about twice as slow (0.019, 0.017), so read them as a range.

To repeat it, inside the Linux image: `cmake -S tools/procmat/sweep -B build/procmat-sweep`, build,
`python3 tools/procmat/expand_nodes.py --out <corpus> --chains` (`--float-hash` for R10's control),
then `procmat-sweep compile`, `interpret`, `crosstier` and `bench` (`EGL_PLATFORM=surfaceless`,
`LIBGL_ALWAYS_SOFTWARE=1`). `crosstier` exits 1 while anything is over AC8, so today it exits 1.

### Deviations

- **R17, import scope.** The tool imports what the non-3D library reaches: 250 descriptors, 391
  functions, 246 entries. The other 152 `.mmg` files are 3D, materials or old versions that nothing
  references (`_manifest.json` lists them). It scans all of them for attributions anyway.
- **R19.** Of the 26 nodes with their own notice, 14 are imported and in `NOTICE.md`. The others are
  3D, deny-listed or unreferenced. The notice table in the tool has 25 of them; the 26th, `hbao`, is
  deny-listed, so it is never read.
- **R10.** The mixing is MurmurHash3's (public domain), since the published GPU hashes carry licence
  questions of their own. Coordinates clamp to ±32767 before quantizing, because a float-to-int
  conversion out of range is undefined. The four `fract(sin(...))` nodes are out: `shard_fbm` by the
  deny-list, the three `tex3d_*` by category. A fifth float hash was found by review, the
  `(34x + 1)x mod 289` permutation of `fbm2` and `fbm4`'s simplex noise, inexact from x = 702. It is
  now integer (`jp_permute289`), with the same results below that. llvmpipe cannot show the original
  problem, so the fix is by reasoning, not by a measured difference. `pixelize` and `wavelet` keep
  their low-gain `fract` terms, which differ only by rounding (inferred).
- **R3, parameter strings.** A graph's parameter values and library presets reach GLSL as text, so the
  importer accepts only numbers, `+ - * / ( ) ,`, and macros in the closed set (`0.005/$n_scale`,
  `$uv.x`). The C++ expander can rely on that for imported data, and must still check `.procmat`
  values. The `Simple/Image` preset's host path is dropped.
- **Import-level fixes the sweep found.** A bare number as the default of a vector port becomes
  `vec4(n)` (`fill_iterate`, `layer_variations_color`, `tiler_color`). `normal2height_base`'s default
  named a macro it does not have. `blend` lost its `linear_light` value, whose function it never
  had. Functions named through macros now resolve (`blend_$blend_type#`, `swirl_tile_$tile`,
  `$direction_$colorspace`).
- **Rules the C++ expander (R5) must keep, from `tools/procmat/expand_nodes.py`.** These differ from
  Material Maker on purpose:
  - Parenthesize coordinates, because `$uv.x` with `a+b` binds to `b`. Where the types still agreed,
    Material Maker silently gave a different image, so looks can differ from it.
  - `.variation()` on an unconnected input passes only its coordinates to the default.
  - A 3D port viewed in 2D gets a local `p`.
  - An input using `.size` or `.texture` needs a Buffer (R6).
  - Every pass ends in `jp_finite` (R12).
- **R11, what the prototype leaves out.** No `discard` or `switch`, because the imported library has
  neither; they fail at load like any other unsupported instruction. So does a malformed module, so
  a corrupt one is an error rather than a crash: every instruction's length, ids, block structure and
  operand shapes are checked, and the finished register code is verified on its own. It also refuses
  images other than plain 2D, and uniform blocks with padding. It runs on plain threads in the
  bench, and only the tests and `procmat-sweep` link it. `SpirvMachine` copies `in` arguments, as GLSL
  does. Dispatch is threaded on GCC and Clang and a plain switch on MSVC, which has not been compiled.
- **AC7.** `SpirvInterpreterTest` computes expected values in C++ from the GLSL definitions. The
  independent Python oracle remains M5's.
- **Inferred, not tested:** `compile_to_spirv` is not reachable from the editor today, because the
  engine links `shadercompiler` privately (`engine/CMakeLists.txt:2205`). M3 or M5 needs an engine
  entry point for it.

### M3 (s90-core)

What M3 decided and where it differs from the text above. Measured on llvmpipe (glcore) in the Linux
image, through `slot`; the evaluator's GPU tests and the per-node image test are in
`engine/tests/core/procmat/`, the rest in `editor/tests/procmat/`.

- **R1.** `AssetType::ProceduralMaterial` is 29: 24-27 are pixel art's and 28 is SoundEvent. The GUID is the
  file's top-level `guid` (an unknown key to S23's model, kept), written by `AssetDatabase` through
  `graph::save`, so the file stays in the graph model's canonical form.
- **R2.** A node's `typeVersion` is stored in the node (S23 keeps it as an unknown key). A version other than
  its descriptor's `type_version` is an error at validation and refused by the planner; no descriptor has a
  second version yet, so there is no migration to select.
- **R3, file names.** A file stores an enum by name (`"blend_type": "multiply"`, from the value's label);
  an index is accepted too. Flags accept 0 and 1, because the library's own presets store them so. Ramps
  hold at most 32 keys. The validator reports wrong types as errors (`param-type`); out-of-range numbers
  stay S23's warnings, and the planner refuses them.
- **R5.** `editor/procmat/ProcMatExpander.*` writes byte-identical shaders and uniforms to
  `tools/procmat/expand_nodes.py` for all 1,410 shaders of the library sweep (`ProcMatExpander.
  AgreesWithThePythonReferenceOnTheLibrary`, against `editor/tests/procmat/ExpanderReference.txt`, which
  `test_import_mm_nodes.py` checks is current). Python stays the reference for the sweep.
- **R6.** The planner cuts at the first of: an output expanded at 4 or more coordinates, an input called
  from a loop, then textures, uniforms and text over M0's limits, at the input whose source holds nearest
  half of what is over. It only cuts colour-typed ports (`f`, `rgb`, `rgba`): a Buffer cannot hold a
  signed distance or a 3D value. An input read as `.texture` or `.size` gets a Buffer with box-filtered
  mips. An inserted Buffer takes the plan's size.
- **R7.** Which edit is a uniform edit is decided by generating the plan and comparing its text and sizes,
  never by a list of parameter kinds. A graph's seed reaches every shader, including those inside library
  graphs, as a uniform.
- **R12.** Buffers are RGBA16F; outputs go through a clamping quantize pass into RGBA8 and are read back after
  the frame that drew them is committed. A preview plan (`readBack = false`) leaves its image on the device.
- **Import fix.** A library graph's parameter linked to a Buffer's flag (its "32 Bits") was typed `size`;
  it is `boolean` now (`import_mm_nodes.py`, regenerated: 8 descriptors).
- **R20, the per-node image test.** `ProcMatNodeImageGpuSlow` plans and evaluates every variant of every node
  for real (Buffers feed passes, Iterate Buffers iterate, half rounding on both sides), at 128² with Buffers
  capped at 64, on the device and on `SpirvInterpreter`. All 250 nodes ship: 1,179 outputs, worst 1.0 mean,
  4 at p99. Its first run put 6 outputs of 4 nodes over AC8; each was traced to a formula whose result is not
  defined, and fixed in the importer (`TEXT_FIXES`, `GLOBAL_TEXT_FIXES` in `import_mm_nodes.py`, which fail when
  Material Maker's text changes). These are deviations from Material Maker, whose GPU draws the same ties its own
  way:
  - `bricks`, output Direction: `sign(width - height)`, zero up to rounding on Running Bond 2's square bricks.
    A difference under 1e-4 is now 0.5.
  - `height_to_angle`: `atan(0, 0)` on a flat input. No slope (`dot(dh, dh) < 1e-12`) is now angle 0.
  - `auto_tones`: its loop keeps an iteration count in a half-float channel as steps of 0.01, so
    `pow(2.0, iter * 100.0)` was no power of two and texel edges fell where each device rounds. The exponent is
    now `floor(iter * 100.0 + 0.5)` through `exp2`.
  - `morphology3`: its dilation keeps the largest neighbour with `>`, and neighbours equal up to rounding (any
    gradient has them) were picked by each device's rounding. A neighbour now wins only when larger by 1/1024,
    under what 8-bit output shows. Its float-stepped loops are integer loops, and `pack_2x32_to_4x16` (shared
    with `fill2` and `slope`) quantizes its low half to 1/2048, which a half holds exactly; neither changed the
    measured difference alone, but both remove a rounding the result depended on.
- **Nearest-texel sampling** (a Buffer with `filter: false`) moves texel edges by 1/64 texel: another grid's
  coordinates and the GPU's interpolated ones landed exactly on an edge, which each device rounds its own way.
  Both expanders do it, and the reference was regenerated.
- **A Buffer's size per step.** Each pass knows where the sizes of the Buffers it reads are in its block
  (`PlannedPass::sizes`), and the evaluator sets the size the Buffer has at that step: a shrinking Iterate
  Buffer halves, which Material Maker's constant `<buffer>_tex_size` ignores.
- **R28.** The kind sets `strictParams` (BF23): a misspelled parameter, an unknown node type and an out-of-range
  value are errors, so `edit_graph` refuses them. A wrongly named enum value is S23's `param-range` error with its
  did-you-mean, not also the kind's `param-type`.
- **R3, bounds.** Images are 1 to 64 pixels a side with exactly the words their size needs and no pixel past the
  palette; shapes hold at most 64 points; a lattice is 1 to 8 cells a side with (x + 1)(y + 1) points.
- **R21.** Not as written: two compiler entry points were added to the engine (`ShaderVariantCompiler::
  compileSources`, `compileFragmentToSpirv`), because the engine links `shadercompiler` privately and the editor
  has no other way to it. They compile only with `JASPERENGINE_RUNTIME_SHADERS`, use no new dependency, and nothing
  in a game calls them; a player's size was not measured (AC18's check is M6's).
- **Not run.** Vulkan (lavapipe) and any hardware GPU: the GPU tests ran on glcore only. Tier selection,
  the CPU tier's evaluator, MCP tools and the window are later milestones.
### M2 (s90-canvas): the graph-editing foundation

- **R26, the window for every kind.** The foundation is a window of its own, **Graph Editor**
  (Window > Assets > Graph Editor, `focus_window graph_editor`), which opens a file of any
  registered kind no other window claims; the Procedural Material window is a subclass with its own
  name and kind (`GraphEditorWindow`'s protected virtuals: thumbnails, cost, preview pane, toolbar
  extras, kind problems, change hooks). Until a kind registers, only the test kind reaches it.
- **Groups and comments.** The model has groups (drawn around their members, a press on the title
  drags them) and no comments, so the canvas draws none.
- **Dirty state.** None, as every asset window: an edit is written at once as one GraphEditCmd
  step; a field's drag folds through its session (the Animator rule).
- **R30.** A node without a position is a model state (`graph::Node::placed`): saved without a
  position, so editors keep laying it out until a move or the `layout` op places it. Opening never
  writes. `layout {ids?}` is in `graph::apply` (so `edit_graph` takes it); its text in
  `edit_graph`'s description is left to M6's budget pass, only its error lists it.
- **Search entries.** `NodeTypeDesc::aliases` is new (kinds fill it from Material Maker's
  aliases). Enum values are entries only once the user types, so the unfiltered list stays one row
  per node type.
- **Ramps.** Gradient and curve parameters are `ParamDesc` kinds `gradient` and `curve` in Material
  Maker's JSON shapes. The Gradient Editor's key limit is a field option (32 for a graph, 8 stays
  the default); a graph gradient whose colour and alpha keys sit at more than 32 places is refused
  with a status line, since a file point holds both. Interpolations 2 and 3 show as Blend and are
  kept unless the mode is changed.
- **Review round 1.** The window's edits follow `edit_graph`'s rule: an edit (a paste, a value, a
  wire) that adds an error issue is refused, and a pasted node takes only its type, place and
  values from the clipboard. `layout` spaces each node by its port rows (`LAYOUT_ROW` 180 plus 20
  per row), room for a 112-unit image. `focus_window` takes `path` for `graph_editor`, so an agent
  can show a human a given graph. A Preset saved from a 32-key graph ramp still reloads as 8 keys
  (the swatch library's gradients are the particles' format); that stays for M4.

### M4 (s90-window): the Procedural Material window

- **The window.** `ProceduralMaterialWindow` is the Graph Editor's kind window (`editor/window/`), opened by a
  double-click on a `.procmat`, by Assets > Create > Procedural Material (a Perlin noise on a Material Output's Base
  Color and Roughness, `assets.createProceduralMaterial`) and by `focus_window {window: "graph_editor", path}` on a
  `.procmat`, which now opens a file in its kind's window as a double-click does (no schema change: core stays at its
  listing). It has no Window menu entry of its own: it always has a file, so it opens from one.
- **The window's own fields are graph metadata**, written by a new invertible op `setMetadata {key, value}` in
  `graph::apply` (so `edit_graph` takes it too; its undo puts a removed key back in place): `seed` (a whole number,
  0 when absent), `seed_locked` (true, or absent), `exposed` ([{node, param}]) and `presets` ([{name, values:
  {"<node>/<param>": value}}]). A bake (R15's recipe, R35's default `seed`) reads `seed` from there.
- **Seed lock.** One lock for the graph's seed: while on, the seed field, Reroll and Variations leave it alone, so a
  bake repeats. Material Maker's lock is per node; a node's own `seed` parameter stays its own here.
- **R24, the thumbnails** (`procmat/ProcMatThumbnails`): one evaluator draws every node image and the Variations
  grid; an LRU of 64 images of 128²; at most one image finished per frame, the selected node first, then the Material
  Output, the Variations, the visible nodes; a value edit of the image the evaluator holds is a uniform edit. An image
  of the device stays on it (`ProceduralGpuEvaluator::takeOutputTexture`); the CPU tier's comes as pixels. Tier 2:
  the selected node and the Material Output only; tier 3: the selected node only, its job cancelled when another is
  selected; the Variations grid is drawn on every tier once opened.
- **The cost on a node** is the time its 128² image took on this device, the node and what feeds it, planning and
  compile included, measured around the evaluator's slices. On tiers 1 and 2 that is the CPU's side of the draws, not
  the GPU's time (inferred: the draws run asynchronously); R36's `cost` block is the measured one.
- **The preview.** Its images are read back at 256² (`PREVIEW_SIZE`), so the ball (one `MaterialRender`) has textures
  on every tier; the 2×2 tile is four quads, since a backend's ImGui sampler need not repeat. A selection's preview is
  a second session on the node's first output (`ProceduralSession::setRoots`).
- **Where the GPU work runs.** The window's sessions and images advance in `App::engineRender`
  (`ProceduralMaterialWindow::processFrame`) and commit what they drew there: a read back waits for the commit of the
  frame that drew, and an editor whose scenes are idle commits nothing else (the first MCP run found the preview stuck
  out of date).
- **The Bake estimate** is the preview's last rebuild (from its debounce to its image, wall clock, so frame pacing is
  in it) scaled by pixels, `t · (size / 256)²`, shown on tiers 2 and 3 as "about N s" (R25: both tiers scale with
  pixels). A value edit's redraw is not timed.
- **Variations** cells take a click only once their image is of their seed (a clock marks the others).
- **Seams to the bake and the tiers** (`ProceduralMaterialWindow::Services`): the evaluator factory, the device image,
  the check report (`{checks, fixes: [{check, node, op}]}`: a fix of one op is the row's Fix) and the bake. Until
  s90-bake and s90-tiers land, the defaults are the GPU evaluator with its tier not claimed (the pane names the
  device, "tier unknown", and R24's tier 2 budget applies, since the device may be a software renderer), no check rows
  and no bake (the button says why). A check FAIL does not refuse a bake; the graph's own errors do.
- **M6's preset finding.** A gradient preset saved from a 32-key ramp reloads with its 32 keys on this base
  (`SwatchLibrary.AGradientPresetKeepsAGraphRampsKeys` passes without a change): the swatch library's native format
  is read without `normalize()`, which only the upstream dialect calls. Dropped.

### M5 (s90-tiers)

What M5 decided and where it differs from the text above. The CPU tier's tests are in
`editor/tests/procmat/` (this binary has no device, so they run anywhere, D3); the GPU tiers against it
are in `engine/tests/core/procmat/`.

- **R8, detection.** `procmat::editorDevice()` (`ProceduralDevice.h`) gives `{tier, name}`: no context is
  `cpu`; Vulkan's `VK_PHYSICAL_DEVICE_TYPE_CPU` is `software`, read through a new
  `SystemRender::isCpuDevice()` (an `sgj_device_is_cpu()` beside `sgj_device_name()`, false on every other
  API); otherwise `GL_RENDERER` or the device name is matched against llvmpipe, softpipe, swrast, lavapipe,
  SwiftShader, Microsoft Basic Render and Apple Software Renderer, and anything else is `gpu`. The CPU
  tier's name is "SPIR-V interpreter on the CPU". `makeEditorEvaluator()` is the one way to get an
  evaluator in the editor.
- **R21.** The device type is a second engine change for S90: three accessor lines in
  `SystemRender`/`SokolSystem` and one in `sokol_gpu_timer_impl.h` (our own extension, not vendored sokol).
  Nothing in a game calls it.
- **R9, D4.** `ProceduralTieredEvaluator` wraps the device's evaluator. A failure to prepare, compile or
  read back moves that evaluator to the CPU tier for good: the evaluation starts over there from the plan
  as it is now (uniform edits included), so neither the device's partial work nor an earlier image reaches
  the result. The result, and every later one, carries `warnings` naming the pass (index and label), the
  device and the compiler log. It never tries a software tier. Which evaluator a new graph gets is the
  caller's: `makeEditorEvaluator()` starts on the device again, since a failed pass is a property of a graph
  more often than of the device (cost if wrong: one failed compile per graph before the fallback).
- **R11.** `ProceduralCpuEvaluator` compiles and loads each pass inside `advance()`, one pass a unit. Each
  step is one Background job (`GateClass::Heavy`) whose rows a `parallelFor` shares, so `advance()` only
  polls; a cancel, a new plan or an edit that affects the running step stops it at its next row, and the job
  keeps what it reads alive. A row past the interpreter's loop limit fails the evaluation naming the pass and
  the pixel. Unlike a device tier, a result holds the outputs' pixels even when the plan does not read back.
- **R11, naming the node.** A pass the interpreter refuses names its node when it inlines one, and "one of
  nodes a, b" otherwise; `ProceduralSession`, which has the graph, then plans each of those nodes alone and
  names the smallest one that is refused (`refusedNode`). The bake path names the pass's nodes only, unless it
  calls `refusedNode` too.
- **Not done (review minors).** D3D11 tells software adapters by name only, not by `DXGI_ADAPTER_FLAG_SOFTWARE`
  (Windows is not built here); `refusedNode` compiles each node of a refused pass on the main thread, which only an
  R11 refusal reaches and the shipped library has none.
- **R12.** A Buffer is stored as RGBA16F stores it, every mip level included (`SpirvImage` with
  `halfPrecision`), and only a Buffer the plan gives mips has a chain: one level samples level 0 at any level of
  detail, as on the device. `roundedToHalf` moved from the test harness into `SpirvInterpreter`.
- **R39, no window.** `procmat::evaluateOnCpu(plan, result, cancelled, progress)` is the entry point for a bake
  that runs in-process with no context: it needs the shader compiler and the job system only, both of which
  work before any backend starts. `ProceduralRun` (s90-bake) should take its evaluator from
  `makeEditorEvaluator()` and its `device` from the evaluator after the run, not from the context, so that a
  fallback shows as `cpu`.
- **AC7.** `tools/procmat/spirv_oracle.py` computes 21 cases (83 input sets) in Python from the GLSL
  definitions, in 32-bit floats and integers: arithmetic, rounding, comparisons with NaN, logic and selects,
  integers and their bits, an unsigned hash, conversions, swizzles and dynamic indexing, matrices, loops with
  `break`/`continue` and an `inout` call, the `GLSL.std.450` set the interpreter covers, and a sampler
  (bilinear, repeat outside [0, 1], box mips blended by the lod's fraction and clamped at both ends, from the GL
  specification's definitions). Exact results are
  compared bit for bit, transcendentals and unordered sums within 4e-6 and 1e-6 relative.
  `SpirvInterpreterTest.MatchesThePythonOracle` reads `SpirvOracleCases.h`, which
  `test_import_mm_nodes.py` checks is current.
- **R20 on lavapipe.** The per-node image test's first run on Vulkan put one output over AC8:
  `height_to_angle` wired, 3.98 mean, 255 at p99. Its probe is a sawtooth in x, so on the 1.4% of pixels at
  its drops the slope points down -x with a zero `dh.y`, and `atan` gives pi or -pi by the sign of that zero,
  which lavapipe leaves otherwise than the interpreter (inferred from the share of pixels; the fix confirms
  it). The importer's fix now makes that cut pi: 0.25 mean, 1 at p99 on both backends. Another deviation from
  Material Maker, whose GPU picks its own sign there.
- **AC8.** `ProcMatCrossTierTest` is `ProcMatNodeImageGpuSlow`: the per-node image test compares the device
  with the product's CPU tier now, not with a test harness, and its red control
  `TheOldFloatHashIsOverTheLimit` puts Material Maker's float `rand` back and requires the limit to catch it.
- **AC4, not all of it.** `gpu` is shown by names only (`ProceduralDevice.ATierComesFromTheDeviceType...`): no
  hardware GPU and no RTX lane exist on this host. `software` is shown on llvmpipe and lavapipe by
  `ProceduralTiersGpu.TheHeadlessDeviceIsSoftware`, and the injected compile and read-back failures by the same
  suite (`JASPER_TEST_PROCMAT_DEVICE_FAIL`, read by `ProceduralGpuEvaluator`). The MCP half (`device` in a
  `--headless` preview) needs `preview_procedural_material`, which s90-bake writes; its test should run a preview
  with `JASPER_TEST_PROCMAT_DEVICE_FAIL=compile:<pass>` in the editor's environment.
- **AC5** is s90-bake's: the CLI's in-process path is `evaluateOnCpu`, and the editor test
  `ProceduralDevice.WithNoContextTheEditorIsOnTheCpuTier` runs it in a process with no graphics device.
- **AC6** runs each graph on job systems of 0, 1 and 5 workers (the evaluator takes a `jobs::JobSystem`, the
  process's by default), at two slicings and through `evaluateOnCpu`. Its red run put rows where they were
  claimed rather than at their own `y`.
