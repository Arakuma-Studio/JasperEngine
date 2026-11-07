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
| T1 | Terrain operations and authoring | One operation engine behind brushes, MCP, Lua and bakes; Create Terrain dialog, overlays, Raw import/export, delta undo, spaced stamps; `edit_terrain`, `visualize_terrain`, richer `sample_terrain`; Lua height queries and runtime edits |
| T2 | Noise graphs and generation | Our own deterministic noise library and versioned Noise Graph asset, the first real `edit_graph` kind; pins on GraphCanvas; erosion; `generate_terrain` as a job |
| T3 | Terrain materials and holes | Top-N layer cap, quality tiers including a mobile tier, macro variation, distance resampling, cliff projection; holes that agree across render, collision, navmesh and picking |
| T4 | Terrain worlds | Tiled worlds past one map: streaming, per-tile collision paging, far-distance overview, per-tile formats, editing across tiles |
| T5 | Foliage upgrade | Placement on jobs, shared mesh parsing, compact grass instances, tree impostors, tree collision, wind |
| T6 | Voxel volumes | Opt-in module: bounded smooth voxel volumes with Transvoxel LOD, caves, overhangs, destruction, terrain integration |

## Programme decisions (every spec honours these)

**Code and names**
- D1. Pure terrain algorithms live in `engine/core/terrain/` (namespace `jasperengine::terrain`), with no
  ECS or render dependencies, so they unit-test without a GPU. Noise lives in `engine/core/noise/`
  (`jasperengine::noise`). Voxels live in `engine/core/voxel/` (`jasperengine::voxel`).
- D2. New code goes in new files. `RenderSystem.cpp`, `MeshSystem.cpp`, `TerrainEditWindow.cpp`,
  `InspectorWindow.cpp` and `WorldTools.cpp` get the smallest hooks possible, because all streams touch them.
- D3. User-facing names: **Terrain**, **Terrain Layer**, **Noise Graph**, **Terrain World**, **Voxel Volume**.

**The seams between specs**
- D4. **T1 owns the operation engine** (`engine/core/terrain/TerrainOps.h`). Every terrain write goes
  through it: brush strokes, MCP, Lua, spline bakes, generation, world tiles. It takes an abstract
  **`TerrainMapStore`** interface (read and write rectangles of height, blend, hole and density data in
  texels, reporting dirty rectangles) so that T4 can supply a tiled store without changing callers.
- D5. **T1 owns the terrain surface query** (height, normal, is-hole) behind one function used by
  physics, navigation, picking, foliage, splines, particles and Lua; it must keep matching the drawn
  surface (S18:531, S26:467).
- D6. **T2 owns `engine/core/noise/`**; T6 uses it for volume generation, and `generate_terrain` writes
  through T1's operations.
- D7. **T3 owns the hole representation and the terrain material shader changes.** T1 exposes
  `cut_holes` and `fill_holes` as operations; T3 makes them mean something in every consumer.
- D8. **T4 owns tiles, streaming and collision paging**, and is implemented after T1 and T3 land.
- D9. Foliage (T5) stays attached to the terrain or world through T1's surface query.

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
- D17. MCP groups: `terrain` for T1 to T5 (raise its limits in `tools/mcp/budget.json`), `voxels` for T6.
- D18. Runtime code that many games will not use is an S34 player module with `ModuleUsage` detection, a
  CMake option and a test: `JASPERENGINE_TERRAIN_WORLD` (T4), `JASPERENGINE_VOXEL` (T6),
  and `JASPERENGINE_NOISE` (T2's runtime evaluator, also used by voxels).
- D19. Shader variant bits are appended at landing, in landing order, as on the pixel-art and Doriax programmes: the
  table in T3 D2 gives names and the order among our streams, not reservations, and a test pins each. Each shader
  change bumps the shader cache version. T3 widens `getDefineBits` to 64 bits so T6 has room. No spec bumps the scene
  version; new fields default. Asset types 24-27 are pixel art's, so ours start at 28.
- D20. Landing order: T1 foundation first (the `engine/core/terrain` seams), then the rest of T1, T2, T3,
  T5 and T6 in parallel, then T4 last.
- D21. Doriax's unlanded `agent-authoring` and `render-correctness` also touch terrain and foliage. We align with their
  specs instead of duplicating them, and whichever lands second merges onto the first. New MCP text follows the lean
  rules (a failing eval case for new text, guide limits, budget ratchet).
- D22. Keep `sculpt_terrain`, pointing agents at `edit_terrain` through its result. Add `qemu-user` to the Android image so ARM
  determinism can be checked here. Asset types: TreeData 29, TerrainWorld 30, VoxelVolume 31, NoiseGraph 32 (upstream's SoundEvent took 28,
  so NoiseGraph moved; only T2 declares NoiseGraph, and the others take it from T2's branch).
- D23. Owner rule: nothing runs a full suite until the very end of the whole programme. Streams run only filtered tests, and
  not `tools/check-changed`. Full gates, the MCP baseline, and web and Android builds run once on the merged tree, by the coordinator.

## How the work runs

- **Everything builds on this host**, at the owner's instruction (this replaces the brief's rule against
  building here; no rented boxes). Docker was installed for this. 8 cores, 15 GB RAM, 32 GB swap, no GPU, no KVM.
- **Build queue:** `/var/lib/orquester/workspaces/jaspersito/je-remote/rb.sh L ...`, a local stand-in for
  the brief's `rb.sh`: 3 slots of 3 jobs, first come first served, shared ccache from the main checkout. `rb.sh Q ...` is a
  fast lane of 2 single-job slots for work under a few minutes.
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
| T1 spec | done | 93 requirements, 73 acceptance criteria at first draft |
| T2 spec | done | 71 and 47 |
| T3 spec | done | 53 and 47, then one requirement added in reconciliation |
| T4 spec | done | 55 and 43 |
| T5 spec | done | 63 and 40 |
| T6 spec | done | 80 and 53 |
| Spec reconciliation | done | One bit table, one landing order, T1's names win |
| Merge of `origin/main` (pixel art, Doriax specs, lean MCP, no CI) | done | Specs renumbered from S41-S46 to T1-T6 |
| Spec revision for the upstream changes | done | `terrain` 10 tools, `voxels` 4, no format bump, no new budget exceptions |
| T1 foundation | done, verification continuing | On integration; writer migration has since moved to `terrain/s53-authoring` |
| T1 implementation | in progress | `.worktrees/s41`. Every brush, bake and generator writes through the engine |
| T2 implementation | in progress | `.worktrees/t2`. Core: 67 filtered tests pass at `c7ded25f3`. Editor half uncommitted, its tests not yet run |
| T3 implementation | in progress | `.worktrees/t3`. Holes committed at `a88e00f3d`, 41 filtered core tests pass; collider tests and Vulkan not run. Tiers and materials next |
| T4 implementation | not started | After T1 and T3 |
| T5 implementation | in progress | `.worktrees/t5`. Runtime, render hooks and editor panel committed; filtered tests running |
| T6 implementation | in progress | `.worktrees/t6`. 110 of 114 filtered tests at last report; GPU tests and the ball test open. Generation and terrain integration not merged |
| Merge `terrain/integration` to `main`, merge `origin/main`, push | not started | |

## Log

### 2026-11-04
- Owner approved all six steps and delegated product decisions. Declined a rented build box: build on this
  host, install Docker.
- Installed Docker, added swap, wrote the local build queue, started the baseline build.
- Wrote this ledger and the programme decisions; started the six spec writers.
- All six specs written and reconciled: shader bits, version policy, hole map scope, Alt key, seam names.
- Started T1 foundation in `.worktrees/s41`.
- Owner flagged remote changes. `origin/main` had 28 new commits: pixel-art specs S41-S52, 19 Doriax specs, lean MCP rules,
  an AGENTS.md rewrite and removal of all CI. Renumbered our specs to T1-T6, merged, restarted the baseline, and started a
  revision of the specs. Doriax's agent-authoring and render-correctness overlap terrain and foliage.
- Baseline: Linux, web and Android builds pass; `HumanoidModel.PerformanceBudget` fails; `CanvasGroupTest.PerformanceBudgets` skips
  (the gate counts it as a failure); the MCP suite did not run (runner refused files "left out of CI").
- The foundation took about 10 hours. Main cause: the coordinator made every stream wait on it, leaving half the host idle
  from 08:03. Stopped waiting: T2, T3, T5 and T6 start now on the foundation's committed work; queue changed to 3 slots of 3 jobs.
- `origin/main` moved 95 more commits and took S53-S73, so the specs moved to their own T1-T6 series. Merged.
- 2026-11-07: the coordinator session restarted and stopped every agent mid-work; all five leads revived from their saved
  state (their own sub-agents are gone; T2's canvas agent had died on an API weekly limit). `origin/main` was 763 commits
  ahead: Doriax render-correctness (foliage ring, Min Draw Distance, Foliage Distance), mesh-memory, ssr-probes and
  build-toolchain landed, plus a header fan-out cut and a generated AssetRefSchema. Merged into integration (conflicts in the
  asset table, reflection, CreateEntityCmd includes and the Xcode project; holeMap now gets its reference row from reflection).
  Each stream merges integration once. Owner repeated: light, filtered suites only.
- Stream findings so far (filtered runs only, nothing is final):
  - T2: erosion's default `radius` of 1 made terrain rougher (one-texel roughness x1.52). Coordinator overrode the spec: default is
    now 2 (x1.04, 9.0 M steps/s on a busy host, budget 8, needs an idle-host bench). File node order is safe because `fromJson`
    sorts; red control run. ARM noise check (`cross_check.sh`, qemu-user: emulation) runs once near the end.
  - T3: `mesh.frag` discards at the end of `main`, because a discard at the top crashed llvmpipe's JIT. Costs extra shading on hole
    pixels on real GPUs, only in hole variants; cause inferred, not proven.
  - T6: fixed a use-after-free in physics prepare jobs, heights scaled twice in the terrain base, and voxelizer edge misses. A ball
    on voxel ground lost about 18% more speed than on a seamless box; trace says rolling on triangles, not seams, so the test now
    compares against one mesh body. Keep that loss in T6's report: it could still show in games.
- `origin/main` is 71+ commits ahead. Merging at landing instead of now, because a merge forces every stream to rebuild.
- Owner asked that nothing runs full suites until the very end. Checked: nothing was. The brief's `check-changed` step could have
  grown into one, so it is banned for streams.
- Queue starved a one-file syntax check for over an hour; made the queue first come first served and added a fast lane.
- Specs revised for no CI, the lean MCP rules and the Doriax overlap. Six near-duplicate tools merged away.
