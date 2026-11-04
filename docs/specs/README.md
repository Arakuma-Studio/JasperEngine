# OpenEngine port — feature specs

Each `SNN-*.md` file here specifies one work stream of the OpenEngine-inspired upgrade of
JasperEngine. The comparison that motivated them is in `docs/openengine-analysis/` (00-SUMMARY.md
plus ten domain reports). OpenEngine's source is available read-only at
`/var/lib/orquester/workspaces/jaspersito/JasperEngine-2/external/OpenEngine`. It is reference material: copy or adapt code where it is good, write better code where
we can. Specs describe WHAT and the acceptance bar, not a step-by-step implementation plan.

## Global decisions (apply to every spec)

- **C++20** is the engine/editor/project standard (S01 lands first). Use C++20 freely (concepts,
  `std::span`, designated initializers, `<bit>`, ranges where they compile on all our toolchains:
  GCC 11 (the Ubuntu 22.04 container), Clang/Xcode, MSVC, Android NDK, Emscripten).
- **Platforms are non-negotiable.** Everything that runs in a game must work on all our graphics
  backends (GL 3.3 core, GLES3/WebGL2, Metal, D3D11, Vulkan via sokol-gfx) and all export targets
  (Windows, Linux, macOS, Android, iOS, HTML5). No feature may *require* compute shaders, SSBOs,
  bindless, indirect draws or 64-bit atomics. A desktop-only fast path is allowed only with a
  working fallback that produces equivalent results.
- **Mobile/web first cost model.** Every effect costs zero when disabled; state per-feature GPU/CPU
  budgets; prefer fragment-shader and CPU (job-system) solutions.
- **Do NOT copy (even if a spec seems to ask for it):** their render graph, GPU-driven culling,
  DDGI, ray-traced shadows, froxel volumetric fog, CBT terrain, GPU grass, FFT ocean as primary
  paths; the C#/CoreCLR runtime; their ECS core (POD-only, memcpy); their Yoga/CSS UI core,
  docking and editor UI toolkit; vcpkg, the WebGPU editor and the npm package; the Unity importer
  DLL; their gameplay visual-scripting prototype; their 10k-line C++ header parser.
  Consequences decided here:
  - "Volumetric fog" = our own fragment-shader raymarch (reduced resolution + temporal-free
    upsample), not froxels. Height fog is analytic. Clouds are a fragment raymarch.
  - "Render-graph viewer" = a *frame pass viewer* over our existing sokol pass list (no render
    graph is introduced).
  - ECS improvements are done to OUR ECS (non-POD components stay supported).
  - Game/editor UI stays on our RmlUi fork + ImGui editor.
  - Vector text: avoid the Slug technique unless its patent status is verified clear; default to
    exact-scale re-rasterization and/or MSDF.
- **Humans and agents.** Every user-facing feature needs: editor UI (ImGui, Unity-like naming and
  behaviour, undoable edits), a Lua API where it is runtime functionality, and MCP tools /
  resources (editor/mcp) so agents can drive and verify it. Update `docs/` and the MCP agent guide.
- **Serialization compatibility.** Old projects must load. New fields get defaults; format changes
  bump the scene/asset version (S03) with a migration.
- **Testing is part of done.** Unit tests (gtest, see S02), MCP tests (`tools/mcp/tests`), golden
  images where rendering changes, and `tools/smoke-test.sh` must stay green.

## Spec template (every spec uses these sections)

1. **Goal** — the outcome in 2–4 sentences, and why.
2. **Scope** — in scope / out of scope (cite the do-not-copy list where relevant).
3. **Current state** — what JasperEngine has today, with file paths; what is wrong with it.
4. **Reference** — the OpenEngine files to study/copy (exact paths), what to take, what to
   improve on.
5. **Requirements** — numbered `R1…Rn`, each testable. Group as Functional / Platform & backends /
   Performance (concrete budgets) / Editor UX (human) / Agent UX (MCP tools, Lua API, CLI) /
   Data & compatibility (formats, versioning, migration).
6. **Design constraints & decisions** — architecture choices made now so implementers don't
   re-litigate them; public API sketches (headers, Lua, MCP tool names + params).
7. **Acceptance criteria** — numbered `AC1…ACn`, each mapped to a concrete verification
   (unit test name, MCP test, golden image, benchmark threshold, manual check described exactly).
8. **Dependencies & conflicts** — specs this depends on / blocks; files likely touched that other
   streams also touch (merge hotspots).
9. **Risks & defaults** — open questions with the default decision taken.

## Index

| ID | Stream | Wave |
|---|---|---|
| S01 | C++20 migration (all targets, exported project templates) | 0 |
| S02 | Build, CI & tests: presets, sanitizers, unity build, Werror, unit-test framework + core tests, CI runs tests, web browser gate | 0/1 |
| S03 | Data safety: atomic writes + retry, file locks, autosave & crash recovery, scene versioning, unknown-data preservation, refuse lossy saves | 1 |
| S04 | Asset identity: GUIDs for every asset, redirects, sub-asset GUIDs, typed dependency graph, missing-asset tracker | 1 |
| S05 | File watcher & external hot reload (textures, models, materials, shaders) with self-write suppression | 1 |
| S06 | Job system & memory: work stealing, priorities, background lane, nested-safe ParallelFor, I/O service, allocators | 1 |
| S07 | ECS performance & features: O(1) add/remove, dense metadata, >64 types, change tracking, parallel scheduler, command buffers | 1 |
| S08 | Reflection tables driving serialization, inspector (generic + multi-edit), MCP and hot-reload | 2 |
| S09 | Core utilities: async multi-sink logger, RAII event subscriptions, tick scheduler with cancel tokens | 1 |
| S10 | Shadows: stable CSM, cascade blend/fade, receiver-plane bias, PCSS soft shadows, static shadow caching | 2 |
| S11 | Materials & lighting: extended PBR lobes + glTF extensions, specular AA, photometric units, hex tiling for terrain | 2 |
| S12 | Atmosphere: height fog, volumetric fog (fragment raymarch), volumetric clouds | 2 |
| S13 | Post FX: VHS, film simulation, halation, .cube LUT import, dynamic resolution | 2 |
| S14 | Texture compression pipeline: KTX2/Basis, per-platform ASTC/ETC2/BC formats, cooked cache, import settings | 2 |
| S15 | Profiling & render diagnostics: GPU pass timings, VRAM panel, frame pass viewer, CPU profiler, MCP render stats | 2 |
| S16 | UI rendering cost & text sharpness (draw batching, exact-scale/SDF glyphs) | 2 |
| S17 | HTML UI widgets: virtualized list/grid/tree/table, tooltips, typed drag-and-drop, Markdown, live tree patch hot reload | 2 |
| S18 | Splines and everything on them: core, editor tool, extrude/roads, placement, walls/fences, terrain flatten/paint, water fill | 3 |
| S19 | Navigation: Recast/Detour tiled threaded bakes, crowds, agents, grid/hex A*, nav-grid brush | 3 |
| S20 | Particles: processor stack, forces, collisions, trails, sub-emitters, render modes, fixed tick, prewarm, scrub; high performance | 2 |
| S21 | Animation: blend spaces, state machine, layers/masks/additive, montages, IK, root motion, events, retargeting, curve editor | 2 |
| S22 | Gameplay: Jolt character controller, buoyancy + CPU waves, input edge queries & action maps | 2 |
| S23 | Scripting: Lua hot reload in Play, native toolchain fingerprint, compile status, script editor menus, graph data model, numeric expressions, console click-to-source, single JSON API description | 2 |
| S24 | Undo: names, depth cap, Esc-cancel, selection undo, one timeline, history panel, play-mode change review | 1 |
| S25 | Editor search & organisation: command palette, layout presets, asset filters/labels, smart folders, settings search, bookmarks, rebindable shortcuts | 3 |
| S26 | Scene view: triangle picking (BVH), root-then-drill, click cycling, surface drop, camera bookmarks, ortho toggle, quad views, measure/rulers, layer filter, camera popup | 3 |
| S27 | Editor tools: colour picker (eyedropper, swatches), curve editor widget, project hub & templates, movie recorder | 3 |
| S28 | Robustness: hang watchdog, Windows minidumps, crash handler in shipped games | 3 |
| S29 | Version control: Git (+LFS locks), SVN, status overlays, commit dialog, semantic scene diff | 3 |
| S30 | Shipping: prebuilt-player export (no compiler), headless player flags, publish/verify pipeline | 3 |
| S31 | Agent tooling: `jasperengine-editor call <tool>` CLI, contributor AGENTS.md/CLAUDE.md | 3 |
| S32 | Asynchronous shader and pipeline creation: GL worker context + warm draws, KHR_parallel_shader_compile, Vulkan pipeline cache, Metal/D3D11 async, stand-ins, warm-up list | 3 |
| S34 | Player size: per-project engine modules (prebuilt variants, Compile switches, Strip Engine Code), smaller Lua bindings, Linux LTO, measured -Os/-Oz | 4 |
| S35 | Agent audio: deterministic game-time audio driver, Play capture, `audio_session` report and sound-QA checks, Recorder on one tap | 4A (sound; spec-first) |
| S36 | Sound library and generator: `analyze_audio`, `generate_sound`, 40-clip essentials, 927-clip CC0 starter library (editor only), audio import settings and provenance in `.meta`, export credits, `search_sounds` | 4A (sound; spec-first) |
| S37 | Sound Events (`.soundevent`, GUID in `.meta`), gameplay triggers, Lua `Audio` API and `SoundHandle`, Sound Browser, `edit_sound_event` | 4A (sound; spec-first) |
| S38 | Loudness: `AudioMeter` (LUFS, true peak), true-peak lookahead `limiter`, new-project mixer and audio defaults | 4A (sound; M1 first) |
| S39 | Music system: tracks and playlists, tempo, bars and beats, quantized transitions with snapshots, stingers, tempo detection | 4B (sound; spec-first) |
| S40 | Timeline audio: audio cues on animation clips for the Animation Player, Animator and montages | 4B (sound; spec-first) |
| S41 | Pixel Perfect Camera: integer scaling with letterbox or expand, project Pixels Per Unit, one pixel-snap rule on CPU and GPU, 90° steps or Rotate On Grid, sub-pixel smoothing, Scene/Game view pixel zoom | 5A (pixel art) |
| S42 | Pixel Art Mode and validator: Pixel Art preset, 2D Pixel Art template, pixel-art texture import (nearest, lossless, alpha clip), `check_pixel_art` with fixes and verdicts, `check --pixel-art` | 5B (pixel art) |
| S43 | Palettes: `.palette` asset, import/export of every common format, Lospec fetch, Palette window and ramp generator, OKLab/CIEDE2000 maths, Palette Swap and Cycling, Palette Quantize, Apply Palette / Reduce Colours | 5A (pixel art) |
| S44 | Aseprite import: lossless `.aseprite` reader and writer, AsepriteSprite assets (sheets, tag clips, slices, user data), the sprite-sheet runtime (`SpriteSheetPool`, frame references, packer), live reimport, Sprite Preview | 5A (pixel art) |
| S45 | Sprite Atlas: texture sprite sheets (Sprite Mode, `name@WxH`, Slicer), Sliced draw mode, `.spriteatlas` lossless packing, incremental builds, legacy inline-frame migration | 5B (pixel art) |
| S46 | Pixel art cleanup: grid recovery, palette snapping, sheet slicing (port of ritmo), craft lints, verdicts and recipes, `fix_pixel_art`/`lint_pixel_art` | 5C (pixel art) |
| S47 | Agent sprite drawing: MCP tools that read, draw, render and diff `.aseprite` sprites, with undoable, conflict-safe edits | 5C (pixel art) |
| S48 | Pixel FX: dither fade, dissolve, ramp recolour, pixel outline and drop shadow, screen transitions, glitch, hit-stop, pixel shake, held poses, contact sheets and effect lints | 5B (pixel art) |
| S49 | Pixel particles: Pixel Art module and Pixel render mode, palette-ramp colour, dithered fade, pixelfx presets, Analytic (closed-form) simulation | 5B (pixel art) |
| S50 | Grid tilemap: Grid and Tilemap Layers, Tileset assets, terrain autotiling, tile colliders, Tile Palette and brushes, Tiled/LDtk import | 5C (pixel art) |
| S51 | Pixel fonts: Bitmap Font asset from grid sheets, BMFont and pixel TTFs, integer scale on the pixel grid, pixel outline and shadow, HTML UI faces, built-in pixel font | 5B (pixel art) |
| S52 | Sprite instancing: one instance format, batched sprites, Sorting Layers and Sorting Groups, Sprite Batch, batch statistics | 5A (pixel art) |
| S53 | Model import and cook: meshopt/quantized/Draco/WebP/Basis glTF, Model Importer (.meta, per platform), lossless and lossy Mesh Compression, baked LODs, Read/Write, Audit window, Build Report, `meshes` MCP group | 5 |
| S54 | LOD and cross-fade: LOD on by default, fixed selection, baked levels, dithered cross-fade, shadow LOD, culled level, instanced LOD, Scene view Draw Modes, Stats, `capture_model_lods` | 5 |
| S55 | Draw submission: automatic GPU instancing (beside the Doriax draw-perf and sprite-batching), constants once per pass, visible lists on jobs, shadow geometry, stream-budget fix, mesh benchmark gate | 5 |
| S56 | Device tiers (GPU database after The Forge), tier overrides and Game view simulation, thermal and memory response, upload pacing | 5 |
| S57 | Lighting after The Forge: screen-tile light lists, capsule shadows, contact shadows, weighted blended OIT, hair lobe | 5 |
| S61 | Window-scoped commands: a key acts on the focused panel; curve and colour keys rebindable | follow-ups |
| S62 | Recorder integrity: no false success, no stand-in frames | follow-ups |
| S63 | Saved files do not depend on what was drawn or loaded (sprite frames, weather map, guard test) | follow-ups |
| S64 | Shader lists complete for closed scenes (`generate_shaders`, `get_build_info`, Play) | follow-ups |
| S65 | Models load whole: no primitive or material dropped, same on the loading thread | follow-ups |
| S66 | Crash leftovers clean themselves up (imports, template copies) | follow-ups |
| S67 | Android and iOS packaging tested end to end, bundletool download | follow-ups |
| S68 | Linked folders: exports ship them, one file one GUID | follow-ups |
| S69 | Every modal dialog can be seen and answered by an agent (`EditorModal`) | follow-ups |
| S70 | Auto-sync settles before the reply; racy stamps verified | follow-ups |
| S71 | Cheap Mesh update flags (measure first) | follow-ups |
| S72 | Build hygiene: header fan-out, sanitizer builds install | follow-ups |
| S73 | Code cleanups: generated AssetRefSchema, frozen parity data, dead code, shader counter | follow-ups |
