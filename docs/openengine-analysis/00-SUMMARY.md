# OpenEngine vs JasperEngine — consolidated findings

> **Note (2026-11-04, BF23 F28).** This report is historical and stays as written; decisions taken since:
> the imnodes node editor in the "Shader graph" line was rejected by `docs/specs/S21-animation.md` D14:
> node graphs draw on our own `editor/window/widget/GraphCanvas`.

Source: 10 domain reports in this folder (01–10), produced by reading both trees. Nothing from
OpenEngine was built or run. OpenEngine is MIT: anything copied keeps their copyright notice
and gets a NOTICE.md entry. Exceptions are flagged in the "Licences" section.

## The one-paragraph picture

OpenEngine is a ~1.5M-line desktop/WebGPU engine (Vulkan/Metal/WebGPU only, C# on CoreCLR,
its own Yoga+CSS UI that the whole editor runs on). It is far deeper in high-end rendering,
world-building (splines, navmesh, FFT ocean, CBT terrain), animation graphs and editor polish.
It is weaker than us on platform reach (no Android/iOS/GLES/WebGL/D3D11), CI (none), prefabs
(root-only blueprints), UI standards coverage, 2D, joints, bidi text and agent-facing MCP
breadth. Much of its high end needs compute/bindless/indirect draws that sokol and our
mobile/web targets can't use, so the best things to take are **CPU-side algorithms,
robustness patterns, and editor UX designs**, not its GPU architecture.

## Bugs/defects in OUR code surfaced by the analysis (fix regardless)

| Issue | Where | Verified |
|---|---|---|
| Scene/prefab/project saves overwrite in place; a crash mid-write truncates the file | `editor/Project.cpp:5696`, `editor/project/ProjectPrefabPaths.cpp:460` | yes |
| Unknown component keys silently dropped on load, lost on re-save; no scene format version | `Stream::decodeComponents` | report 02/08 |
| CI builds 6 targets but runs no tests | `.github/workflows/ci.yml` | yes |
| Help > Shortcuts says "T toggles local/global"; code binds T=Rect, X=toggle, Q=Hand unlisted | `editor/App.cpp:931` vs `editor/window/MainToolbar.cpp:230-235` | yes |
| README claims terrain "clipmap LOD"; code is a CDLOD quadtree | `README.md` | report 03 |
| Thread pool deadlocks on nested waits (`MeshSystem.cpp:1111` works around it with `std::async`) | `ThreadPoolManager` | report 02 |
| Any MCP client can run every tool (undo, delete_asset) with no approval gate | `editor/mcp` | report 09 |

## Where WE are ahead (don't regress, don't "fix" toward them)

- Platform reach: 6 export targets, 5 backends, mobile/web without compute; 6-target CI with release artifacts.
- Prefabs: nested, variants, per-entity overrides (theirs: root-only `[blueprint]` include).
- MCP: 140 in-process tools vs their 110 through an unauthenticated socket; batch+grouped undo, jobs, path addressing, ~549 tests.
- UI: RmlUi-based — grid, @media, @keyframes, filters, data binding, Lua DOM, Lottie, 661 golden images.
- Terrain sculpting (16 modes incl. real erosion), 9 PBR layers, per-view LOD; 2D lighting & 2D physics; 19 joint types (they have none); bidi text & font fallback; touch/gamepad DB.
- Lua/C++ completion, LuaLS stubs, clangd, sandboxed Lua check.

## Ranked roadmap of things to copy/adapt

Value H/M/L, effort S/M/L/XL. "Source" paths are under `external/OpenEngine/`.

### Tier 1 — quick wins (S effort, high value)

| # | Item | Source | Notes | Report |
|---|---|---|---|---|
| 1 | Atomic save (temp + rename w/ retry) for scenes/prefabs/project/settings | `Engine/Modules/FileSystem` (`PublishFile`, `RenameWithRetry`), `SettingsStore.cpp` mac branch | Extend our `ImportManifestWriter::writeFileAtomic` | 02, 08 |
| 2 | Stable cascaded shadows: texel snap, sphere fit, cascade blend, distance fade, receiver plane | `Rendering/.../ShadowMapRenderFeature.cpp:179-190,545`, `shadow_cascade_blend.glsl`, `shadow_distance_fade.glsl` | Pure math | 01 |
| 3 | Analytic height fog as a 4th fog mode | `height_fog_term.glsl` | Runs on GLES | 01 |
| 4 | Console click-to-source (Lua/C++ error formats → code editor) | `Apps/Editor/Source/ScriptEditor/SourceLocation*.cpp` (~170 lines) | | 05 |
| 5 | Named layout presets (save/switch/import/export) | design from `EditorApplication_LayoutPresets.cpp` | ImGui ini-to-memory | 07 |
| 6 | Contributor AGENTS.md / CLAUDE.md / `.claude/rules` | their root AGENTS.md, CLAUDE.md | Structure only, write our own text | 09 |
| 7 | Native-script toolchain fingerprint check | `GE_ToolchainFingerprint` in `NativeScriptingABI.h` | Compare in `Conector::connect` | 05 |
| 8 | Spline core (prerequisite for roads/walls/placement) | `Engine/Modules/Spline` (~1k lines, CPU) | | 03 |
| 9 | Buoyancy + CPU wave evaluation on Jolt | `Ocean/Source/Systems/OceanBuoyancySystem.cpp` | Mirror our water shader's `waterWaves` | 03 |
| 10 | Curl noise + Noise/Drag/LimitSpeed/Orbit particle modifiers | `Engine/Modules/Noise`, Particles processors | | 03 |
| 11 | Web smoke gate in CI (headless Chrome: console, screenshot, centre pixel) | `Tools/Web/browser_gate.py`, `serve.py` | Plus: run existing tests in CI | 10 |
| 12 | `get_render_stats` + frame-time history MCP tool | pattern from `mcp/src/.../profiling.ts`, `FramePhaseReport.cpp` | Exposes existing `Engine::FrameStats` | 09 |
| 13 | Markdown → HTML for htmlui; NumericExpression in number fields | `UI/Source/Parsers/Markdown*`, `Modules/NumericExpression` (+tinyexpr, zlib) | | 06 |
| 14 | Sanitizer presets + ASan/UBSan CI job; headless player flags (`--quit-after --fixed-fps --screenshot`) | `cmake/`, `Apps/Player/Source/PlayerRunOptions.cpp` | | 10 |

### Tier 2 — core investments (M effort, high value)

| # | Item | Source | Report |
|---|---|---|---|
| 15 | **Picking overhaul**: per-mesh BVH triangle picking, click-cycling through overlaps, root-then-drill selection, surface-hit placement. Flagged by 3 analysts | `Engine/Modules/MeshPicking/MeshBvh` (SIMD + scalar), `SceneViewController.cpp:1695`; later `Modules/SceneBvh` | 02, 04, 07 |
| 16 | **Command palette** (Ctrl+K) with their fuzzy scorer, reused for Add Component / Hierarchy / asset search | `UI/UniversalSearchProvider.cpp:74-130,421-474` | 07 |
| 17 | Autosave + crash recovery for scenes | `Source/Scene/SceneBackupRecovery.cpp`, `TickAutoSave` | 08 |
| 18 | Scene format version + preserve unknown components + refuse lossy save; did-you-mean | `UnresolvedComponentStore`, `SceneLoadSkip`, `SceneIO.h:60-100`, `NearestName.h` | 02, 08 |
| 19 | Undo: command names, depth cap, Esc-cancel drag, history panel | `UndoRedoService.h`, `UndoHistoryPanel.cpp` | 08 |
| 20 | MCP safety: permission modes, per-call approve/deny, named per-agent undo, "undo this turn", audit log | `Packages/ai-assistant` (`AssistantGate.cpp:277-345`, `AssistantMode`, `AssistantTurnUndo`) | 09 |
| 21 | Shell CLI `jasperengine-editor call <tool>` with schema-derived flags, exit codes, dry-run, batch | `mcp/src/cli/{args,output,raw}.ts` | 09 |
| 22 | File watcher + self-write echo suppression (+ inotify backend) → texture/model hot reload | `Modules/FileWatcher`, `ExpectedWriteLedger` | 02 |
| 23 | Jolt character controller (stairs/slopes) | `JoltPhysicsBackend.cpp:578-622`, `CharacterControllerSystem.cpp` | 04 |
| 24 | Animation events (glTF extras), blend-space 1D/2D weights | `AnimationEvent.*`, `BlendSpace1D/2DNode.cpp` | 04 |
| 25 | Input edge queries (pressed/released this frame) + minimal action maps | design from `InputSystem.h` | 04 |
| 26 | Extended PBR (clearcoat, sheen, anisotropy, iridescence, spec AA) + glTF extensions | `standard_pbr.glsl`, `iridescence.glsl`, `specular_aa.glsl` | 01 |
| 27 | KTX2/UASTC → BC7/ASTC/ETC2 texture pipeline (big win for mobile/web) | `Tools/TextureCompiler`, `TextureService.cpp` (libktx, Apache-2.0) | 01 |
| 28 | Navmesh: Recast/Detour wrapper + grid/hex A* + crowd avoidance | `Pathfinding/Source/DetourNavMap.cpp`, `ObstacleAvoidance.cpp`, `GridMap.cpp`, `AStarSolver.cpp` (recastnavigation, zlib) | 03 |
| 29 | Asset browser filters (type/ext/path/label), coloured labels; shortcut catalog w/ conflict detection | designs in `Search/`, `EditorApplication_Shortcuts.cpp` | 07 |
| 30 | Lua hot reload during Play (compile-first, keep old on failure, migrate fields by name) | design from `Managed/HotReload` | 05 |
| 31 | Virtualized list/tree element; tooltip/popup placement (`title` support) | `VirtualWindowCore`, `DropdownPopupPlacement.cpp`, `TooltipOverlay.cpp` | 06 |
| 32 | Release pipeline: manifest/sha256, arch checks, symbol split, SteamPipe, Steam Deck deploy | `Tools/Scripts/publish_game.py` (stdlib only) | 10 |
| 33 | Hang watchdog + Windows minidumps; ParallelFor safe in workers + priority lanes | `Diagnostics/MainThreadHangWatchdog.*`, `Apps/Editor/Source/main.cpp:58-175` | 08, 02 |
| 34 | EZ-Tree procedural trees; hex-tiling terrain textures | `Packages/eztree/Native/EZTree` | 03 |

### Tier 3 — strategic (L/XL)

- **Prebuilt-runtime export** (stage content beside a prebuilt player; no compiler needed for users) — 10.
- **Package/plugin system**: manifests, semver, lockfile, package manager panel (`PackageVersion.cpp` drops in) — 05.
- **Shader graph**: their annotated-GLSL node library + compiler, new ImGui node editor (imnodes) — 01, 05.
- **GUIDs for all assets + dependency graph** — 02.
- **HDR pipeline** (float scene target, auto-exposure, proper ACES/AgX) — 01.
- **VCS integration** (status overlays, lock, semantic scene diff) — 08.
- **In-editor assistant driving the user's `claude`/`codex` CLI** with our MCP attached (no API keys) — 09.
- **Platform services interface** (saves/achievements/entitlements; base for Steamworks; move AdMob/CrazyGames out of `System.h`) — 10.
- **Slug vector text** — 04, 06. Check the patent status first.
- Spline geometry (roads/walls/terrain flattening), FFT ocean (needs compute; desktop-only tier).

## Do not copy

Render graph, GPU-driven/HZB pipeline, DDGI, RT shadows, froxel volumetrics (need indirect/compute/bindless
that sokol + GLES/WebGL can't do) · C#/CoreCLR stack · their ECS core (POD-only, memcpy) · their Yoga/CSS UI
core, docking, editor toolkit · vcpkg setup, WebGPU editor, npm package · Unity import (prebuilt .NET DLL) ·
their gameplay visual-scripting prototype · their 10k-line C++ header parser. Also: several of their
animation features are stubs (controller transitions unused, 8/9 skeleton modifiers empty). Port only verified code.

## Licences to watch

- MIT (default): keep notice, add NOTICE.md entry. `SceneBvh` carries a second MIT notice (speedball-gi); Ocean derives from Crest (keep its NOTICE); shader graph nodes need Godot MIT attribution.
- **MPL-2.0**: terrain erosion filter (`TerrainECS/.../Erosion/ErosionFilter.*`). Keep separate or skip; we already have erosion.
- **Apache-2.0**: Bend contact shadows, Khronos PBR Neutral, metal-cpp, libktx. **BSD-3**: ACES2/OpenColorIO. **zlib**: tinyexpr, recastnavigation. **Unlicense**: fog particles. **BSD-2/Boost**: concurrentqueue.
- Unverified: their 216 SVG editor icons (no licence file), some .NET package licences, Slug patent.
- Their code is C++20; we default to C++17 (build already allows 20), so ports need small backports.
