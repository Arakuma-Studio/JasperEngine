# Terrain programme ledger

The running log of the terrain programme: what we are building, what is decided, what is in
progress, what is left and what is done. Updated as work happens; the newest log entry is on top.

## Goal

Make JasperEngine's terrain the best part of the engine for both people and AI agents, taking the
good ideas of Terrain3D, godot_voxel and FastNoise2 and keeping our own rules (all five graphics
backends, all six export targets, no compute/SSBO/bindless/indirect, a disabled feature costs
nothing, old projects load).

The owner answered yes to both open product questions: games need **worlds bigger than one map** and
**caves and destructible ground**. So all six steps are in scope, and product decisions are delegated
to whoever writes the spec ("what most users and agents expect of a game engine; be braver").

## Scope: the six specs

| Spec | Step | One line |
|---|---|---|
| S53 | Terrain operations and authoring | One operation engine behind brushes, MCP, Lua and bakes; Create Terrain dialog, overlays, Raw import/export, delta undo, spaced stamps; `edit_terrain`, `visualize_terrain`, richer `sample_terrain`; Lua height queries and runtime edits |
| S54 | Noise graphs and generation | Our own deterministic noise library and versioned Noise Graph asset, the first real `edit_graph` kind; pins on GraphCanvas; erosion; `generate_terrain` as a job |
| S55 | Terrain materials and holes | Top-N layer cap, quality tiers including a mobile tier, macro variation, distance resampling, cliff projection; holes that agree across render, collision, navmesh and picking |
| S56 | Terrain worlds | Tiled worlds past one map: streaming, per-tile collision paging, far-distance overview, per-tile formats, editing across tiles |
| S57 | Foliage upgrade | Placement on jobs, shared mesh parsing, compact grass instances, tree impostors, tree collision, wind |
| S58 | Voxel volumes | Opt-in module: bounded smooth voxel volumes with Transvoxel LOD, caves, overhangs, destruction, terrain integration |

## Programme decisions (every spec honours these)

**Code and names**
- D1. Pure terrain algorithms live in `engine/core/terrain/` (namespace `jasperengine::terrain`), with no
  ECS or render dependencies, so they unit-test without a GPU. Noise lives in `engine/core/noise/`
  (`jasperengine::noise`). Voxels live in `engine/core/voxel/` (`jasperengine::voxel`).
- D2. New code goes in new files. `RenderSystem.cpp`, `MeshSystem.cpp`, `TerrainEditWindow.cpp`,
  `InspectorWindow.cpp` and `WorldTools.cpp` get the smallest hooks possible, because all streams touch them.
- D3. User-facing names: **Terrain**, **Terrain Layer**, **Noise Graph**, **Terrain World**, **Voxel Volume**.

**The seams between specs**
- D4. **S53 owns the operation engine** (`engine/core/terrain/TerrainOps.h`). Every terrain write goes
  through it: brush strokes, MCP, Lua, spline bakes, generation, world tiles. It takes an abstract
  **`TerrainMapStore`** interface (read and write rectangles of height, blend, hole and density data in
  texels, reporting dirty rectangles) so that S56 can supply a tiled store without changing callers.
- D5. **S53 owns the terrain surface query** (height, normal, is-hole) behind one function used by
  physics, navigation, picking, foliage, splines, particles and Lua; it must keep matching the drawn
  surface (S18:531, S26:467).
- D6. **S54 owns `engine/core/noise/`**; S58 uses it for volume generation, and `generate_terrain` writes
  through S53's operations.
- D7. **S55 owns the hole representation and the terrain material shader changes.** S53 exposes
  `cut_holes` and `fill_holes` as operations; S55 makes them mean something in every consumer.
- D8. **S56 owns tiles, streaming and collision paging**, and is implemented after S53 and S55 land.
- D9. Foliage (S57) stays attached to the terrain or world through S53's surface query.

**Rules from the analysis**
- D10. No FastNoise2 at runtime or in the editor: its cross-target output is unproven, and encoded trees
  have no version field. Our noise is bit-identical across targets with golden hashes (no contraction, no
  libm in kernels), as `engine/core/math/noise` already does for curl noise.
- D11. Keep our CDLOD, do not port Terrain3D's clipmap. It already morphs and works in every render path.
- D12. `sg_update_image` must supply every slice and runs once per image per frame, so streaming uses one
  texture per tile rather than a shared array, or a recorded sokol patch if measurement demands it.
- D13. No integer textures (none in `ColorFormat`, none in shaderlib). Control and mask data use RGBA8 or
  R8 with `texelFetch`. No float-bits packing.
- D14. Transvoxel tables come from Eric Lengyel's MIT repository, not godot_voxel's more restrictive file
  header. Everything copied keeps its header and gets a `NOTICE.md` entry. Godot code is never lifted;
  ideas and formulas are reimplemented, and MPL-2.0 erosion code stays banned.
- D15. Agents get declarative, deterministic, verifiable tools: `dry_run`, seeds, absolute values,
  all-or-nothing validation, changed rectangles and stats in the result, one undo step per call,
  overlays and framing for looking at the result. No rate-based "repeat the stroke" arithmetic.
- D16. Humans get Unity-like names and undo for everything, plus Terrain3D's editor grammar
  (operation-coloured brush decal, Ctrl and Shift modifiers, live info panel, pick from terrain, overlays).
  Alt+drag always orbits by default, as in Unity. Alternates are a button and the `C` key, and the
  preference `Alt Alternates While Painting` (off by default) gives Terrain3D's Alt. Alt+click still picks.
- D17. MCP groups: `terrain` for S53 to S57 (raise its limits in `tools/mcp/budget.json`), `voxels` for S58.
- D18. Runtime code that many games will not use is an S34 player module with `ModuleUsage` detection, a
  CMake option and a test: `JASPERENGINE_TERRAIN_WORLD` (S56), `JASPERENGINE_VOXEL` (S58),
  and `JASPERENGINE_NOISE` (S54's runtime evaluator, also used by voxels).
- D19. Shader variant bits are appended at landing, in landing order, as on the pixel-art and Doriax programmes: the
  table in S55 D2 gives names and the order among our streams, not reservations, and a test pins each. Each shader
  change bumps the shader cache version. S55 widens `getDefineBits` to 64 bits so S58 has room. No spec bumps the scene
  version; new fields default. Asset types 24-27 are pixel art's, so ours start at 28.
- D20. Landing order: S53 foundation first (the `engine/core/terrain` seams), then the rest of S53, S54, S55,
  S57 and S58 in parallel, then S56 last.
- D21. Doriax's unlanded `agent-authoring` and `render-correctness` also touch terrain and foliage. We align with their
  specs instead of duplicating them, and whichever lands second merges onto the first. New MCP text follows the lean
  rules (a failing eval case for new text, guide limits, budget ratchet).

## How the work runs

- **Everything builds on this host**, at the owner's instruction (this replaces the brief's rule against
  building here; no rented boxes). Docker was installed for this. 8 cores, 15 GB RAM, 32 GB swap, no GPU, no KVM.
- **Build queue:** `/var/lib/orquester/workspaces/jaspersito/je-remote/rb.sh L ...`, a local stand-in for
  the brief's `rb.sh`: 2 slots, `JOBS=4` each, shared ccache from the main checkout.
- **Branches:** `terrain/integration` collects finished work and merges to `main` at the end. Each spec has
  its own worktree under `.worktrees/` and branch `terrain/sNN-*`.
- **Evidence** stays outside the repository in `/var/lib/orquester/workspaces/jaspersito/terrain-evidence/`.
- **Known verification limits of this host:** no Windows, macOS or iOS builds; no Android emulator (no
  KVM); software GL only; no `fixture-unity` project (those MCP tests skip). There is no CI upstream any more, so
  nothing else checks a push; anything not run here stays unverified and is reported that way.

## Status

| Item | State | Notes |
|---|---|---|
| Analysis of the three repos and our code | done | Nine reports |
| Build host setup | done | Docker 29.1.3, swap, `rb.sh` queue |
| Baseline on the merged tree | in progress | Linux, unit, MCP, web and Android results to compare against |
| S53 spec | done | 93 requirements, 73 acceptance criteria at first draft |
| S54 spec | done | 71 and 47 |
| S55 spec | done | 53 and 47, then one requirement added in reconciliation |
| S56 spec | done | 55 and 43 |
| S57 spec | done | 63 and 40 |
| S58 spec | done | 80 and 53 |
| Spec reconciliation | done | One bit table, one landing order, S53's names win |
| Merge of `origin/main` (pixel art, Doriax specs, lean MCP, no CI) | done | Specs renumbered from S41-S46 to S53-S58 |
| Spec revision for the upstream changes | in progress | |
| S53 foundation | in progress | Seams everything else builds on |
| S53 implementation | not started | |
| S54 implementation | not started | |
| S55 implementation | not started | |
| S56 implementation | not started | After S53 and S55 |
| S57 implementation | not started | |
| S58 implementation | not started | |
| Merge `terrain/integration` to `main`, merge `origin/main`, push | not started | |

## Log

### 2026-11-04
- Owner approved all six steps and delegated product decisions. Declined a rented build box: build on this
  host, install Docker.
- Installed Docker, added swap, wrote the local build queue, started the baseline build.
- Wrote this ledger and the programme decisions; started the six spec writers.
- All six specs written and reconciled: shader bits, version policy, hole map scope, Alt key, seam names.
- Started S53 foundation in `.worktrees/s41`.
- Owner flagged remote changes. `origin/main` had 28 new commits: pixel-art specs S41-S52, 19 Doriax specs, lean MCP rules,
  an AGENTS.md rewrite and removal of all CI. Renumbered our specs to S53-S58, merged, restarted the baseline, and started a
  revision of the specs. Doriax's agent-authoring and render-correctness overlap terrain and foliage.
