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

- **Build boxes (owner, 2026-11-08):** three rented Hetzner shared-vCPU `cpx62` boxes (16 vCPU, 30 GB, about $0.24/h each,
  48 h deadline): `db9262097667` for T1 (s41), `e704f0707dc6` for T4, `22b24a0ef193` for the upstream merge and the final
  gates. `rb.sh L` runs a mapped worktree's job on its box (je-remote/boxes.conf, rb-remote.sh), 2 jobs of 7 per box, syncing
  tracked and unignored files and copying back what the run wrote. Destroy them when the programme lands.
- **Fewer verification rounds (owner, 2026-11-08):** verify once per batch, with red controls in the same pass.
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
| T1 implementation | landed except pen readers and benches | Last batch merged: averaging alternate, Alt Alternates While Painting, Alt+click on release (T5's tree pick aligned), Layer and Noise Size eyedroppers, real-mouse stroke-log test, brush contexts in list_shortcuts. D-27 to D-37 all built. Pen readers and benches land from terrain/t1-benches |
| T2 implementation | landed | Merged into integration (6af5f3b9d) at 2b280b147: core 133/133, editor 70/70 and 9/9, T2 and T1 MCP files pass, fan-out worst 129, lints. Not run: the test_noise_export player run, je-bench-noise, eval, web, Android, size checks, NDK armv7. Build tree deleted |
| T3 implementation | landed | Merged into integration (9f3ffd6ba) at 8376cd9c4: core 560/560 (all 207 variants), editor 182/182, terrain MCP 35/35, fan-out worst 127. Driver workarounds recorded (llvmpipe GL path). Not run: web, Android, full Vulkan suite, perf checks |
| T4 implementation | in progress | `terrain/t4-worlds` at 02f215f48 (integration with T2 merged). Built: tiles, tiled store, streaming, compound-shape collision paging, `Twd` variant, .terrainworld asset and cook, panel, inspector, undo, Lua, module, edit_terrain_world, sample_terrain {world}. Core 565/565, GPU (lavapipe) 418/418, editor 108/108, world MCP 11/11. Sent back to build: foliage on worlds, Convert to Terrain World, navigation, panel import and cancellable import job, streaming overlay, spline water fill, Physics Placement |
| T5 implementation | landed | Merged into integration (caf5a42a0) at 34b6bf7a3: core 221/221, GPU 22/22 cold cache, editor 137/137, foliage/terrain/navigation MCP files, lints. Not run here: test_foliage_export (player), the eval case for `wait`, web, Android, other platforms. Build tree deleted for disk |
| T6 implementation | landed | Merged into integration (e1021e82c) at 10bc59d4c: core 461/461 WERROR, editor 267/267 WERROR, test_voxels 12/12, voxel and hole GPU tests on Vulkan 48/48 and glcore 49/49. The Vulkan 'hang' was T6's own test giving up after 4 frames while Vulkan creates pipelines on a worker; no render bug (general runtime variant switch on Vulkan verified). Not run: web, Android, sanitizers, full suites, eval. Build tree deleted |
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
- 2026-11-07 04:48: all five leads stopped on a revoked API login (not their work) and were revived. `origin/main` gained 338
  commits, including Doriax agent-authoring (`edit_terrain_details`, `TerrainDetailRules`, the terrain guide), merged into
  integration (one conflict in docs/testing.md). T1 and T5 merge it now because they overlap it; T2, T3 and T6 at landing.
- Verified so far (filtered runs): integration's core filter 84/84 after the first upstream merge; T1's collider refresh
  (29bb7a78b) 162/162 with WERROR; T6 voxel core 144/144 on its merged tree; T2 Lua API 12/12. T2 found and fixed a vacuous MCP
  test and moved the graph-shipping rule into `Exporter::shipsForNoise` with unit tests; its player-level test runs at the end.
- NoiseGraph is asset type 32 (upstream's SoundEvent took 28).
- T1 has become the bottleneck: its unlanded work (101 files) conflicts with T2 (a duplicate `TerrainMapDeltaCmd`,
  `TerrainNoise.h`, `TerrainMaps.cpp`) and T3 (holes across the operation engine). Decision: T1 lands a stable, tested checkpoint
  in integration soon; T2, T3 and T6 then merge integration once and resolve against it. T1's delta command wins over T2's.
  T6's AC35 and T3's move of the hole rule into `sampleHeightField` wait for that.
- Disk filled to 100% on 2026-11-07 ~06:00; freed 35 GB (coordinator's baseline trees, Docker build cache). The queue now waits
  while under 15 GB is free. MCP test runs need `MCP_SKIP_WEB_BUILD=1`, or `MCP_SUITE=all` builds a web project first.
- T6 decision: where a voxel volume meets terrain at a cave mouth there must be no upward step over 2 cm along any path into
  the mouth; the voxel surface meets the terrain edge exactly in render and collision. A 0.25 m lip (the cut tolerance) is a bug.
- Follow-ups outside the programme (for their owners, not fixed here):
  - `SplineGpu.FenceAndWall` fails deterministically on integration's engine code alone (6612 pixels over threshold, fence rails
    about 1.5 px lower; posts and wall match), proven by T3 building integration's exact `engine/`. T1's passing run predates the
    upstream merge, so it came with upstream (likely render-correctness or mesh-memory). The final baseline checks it on plain
    `origin/main`; it is not a terrain-programme regression and is reported to that work's owner. `RagdollWizardDialog::instance().show()` is drawn only from
  the Inspector, the same bug T2 found and fixed for Create Terrain (a dialog opened from another tab is never drawn).
- Shader undefined behaviour or driver bug, found by T3's merged MCP run: a new terrain limits layers per pixel (R53), and that
  variant at High tier and High Lit Quality (the editor default) had never been drawn by any GPU test. llvmpipe kills it with an
  illegal instruction. A rewrite of the layer selection didn't fix it; checks at 128-bit llvmpipe width, on lavapipe and with
  glslangValidator/spirv-val decide whether it's our shader or the driver. T3 adds a GPU test drawing every variant combination
  on glcore and Vulkan. Also: `sample_terrain` could pass the 1 MB result limit with many points; capped at 64 full samples.
- Header fan-out passed 150 on integration after T1's checkpoint (about 153 on `Transform.h`). T1 cut it at the source on an
  isolated branch `terrain/t1-fanout` from the checkpoint; T3 moved its one unit to a narrow header.
- The build queue's first-come rule let newcomers win freed slots when fewer jobs waited than there were slots (T5 waited 144 min
  behind later jobs). Now strictly head of line.
- Editor trap found by T2: `jobs::postToMain` drains only in `Engine::runFrame`, which the editor runs only in frames that draw a
  scene, so an editor job's result can sit forever in an idle editor (T2's first Preview hung this way). Editor-side job
  hand-backs use `EditorHost::enqueueMainThreadTask`, which runs every frame and wakes the loop; T1, T5 and T6 are checking theirs.
  T2's fix has red controls for the idle hand-back and for a throwing job. T2 wires its presets into T1's Create Terrain dialog.
- T1 window batch landed (3b8f715e7), identical to T1's tree. Its test run caught Esc both cancelling a stroke and putting the brush
  down, turning the release into a stray Deselect All; fixed before landing.
- Guide budget resolved on terrain/upstream-merge (aac58b2c9): the programme's two "Other areas" entries removed, 7 bytes of upstream
  wording trimmed with the same meaning, and the terrain and voxels group descriptions now name edit_terrain/edit_voxels and their docs.
- origin/main merge prepared on terrain/upstream-merge (13cf25705): 622 commits, WERROR build, core 810, editor 418 (2 failures,
  one fixed), MCP 123/123, fan-out 148 of 150; SplineGpu.FenceAndWall now passes (upstream recaptured its golden; follow-up
  closed). Found: T3's 12-layer variants use 226-241 GLES3 fragment uniform vectors against 224 (would break on WebGL2/Android;
  T3 fixing on terrain/t3-gles3-fit); VoxelTerrainGpu.CaveMouth fails when repeated in one process (T6 fixing on
  terrain/t6-cavemouth-flake); agent guide 53 bytes over budget (trimming our lines; eval deferred to the final gate); a forced
  terrain tier shared bits with upstream lighting bits (fixed in the merge). The Xcode navigation check fails the same on origin/main.
- T1 final batch part 1 landed (b6312c623). The rest of T1 split in two: window items stay with T1; pen readers and benches go
  to a second agent (terrain/t1-benches), since they live in their own files. The remote queue's false exit 1 came from editing
  rb-remote.sh while a job ran it; jobs now run a private copy.
- Owner allowed rented build boxes and asked for fewer verification rounds. Three Hetzner boxes seeded with the repo, the build
  images and the ccache; the build queue sends mapped worktrees' jobs there. Freed 25 GB of leftover Android build output.
- T1 batch 1 landed (d822fb140), identical to T1's tested tree.
- Sun 18:51: only two streams still building (T1's last batches, T4), leaving a build slot idle. Started the origin/main merge
  (622 commits) in parallel on `terrain/upstream-merge` (.worktrees/upstream), so the final landing doesn't wait on it.
  T4 has 271 uncommitted files; asked twice to commit them as WIP.
- T6 landed (e1021e82c). The suspected Vulkan bug was not one: T6's test gave up 4 frames after the volume opened its holes,
  while Vulkan creates pipelines on a worker thread. A new runtime hole-cut test passes on glcore and Vulkan. All five original
  streams' work is now in integration except T1's remaining requirements; T4 is building.
- (superseded) Vulkan bug found by T6: a runtime switch to a new shader variant (a terrain gaining holes after the first frame) never
  completes on Vulkan; the new pipelines stay in sokol's ALLOC state. T3's hole tests pass on Vulkan only because they set
  holes before the first frame. Decision: fix before landing, scope first (general async-pipeline bug or terrain-specific).
- Integration issues found by T6's merge: T2's unused function in NoiseGraphWindow.cpp breaks WERROR (fixed in T6's branch);
  T5's TerrainFoliageGpuTest.cpp:216 breaks a WERROR core-test build (T6 fixing); locators_expected.txt lacked T5's treeData;
  T3's TerrainDetailBuilder and TerrainRenderInfo.h were missing from the Xcode project. Fan-out worst is now 144 of 150
  (TerrainSurfaceLayer.h); T4 told to keep its headers to forward declarations.
- T2 landed (6af5f3b9d), merged code identical to T2's tested tree. Generation's layer output now reaches all 12 layers.
  Remaining: T6 (merges integration for T2's noise), T1's open requirements, T4.
- T3 landed (9f3ffd6ba), merged code identical to T3's tested tree. It also fixed T5's duplicate `minDrawDistance` reflection
  field, T5's foliage reading layers 9-11 from the wrong channel, and T5's includes after the fan-out cut. T4 started in T3's old
  worktree, reusing its build tree. T2 lands next, then T6.
- T1's fan-out cut landed (a6a3c6538): worst header 123 of 150 (Transform.h was 153, TerrainComponent.h 143, now 113), with
  TerrainSurfaceLayer in its own header. The coordinator's one-file syntax check of T5's sources after the merge was
  unreliable: it borrowed a compile command from a stale build tree, and passed RenderSystemFoliage.cpp, which T3's real build
  then showed needs the Transform include. T3 added it, plus the includes for three T5 test files (48c22c1e3). T5's 11 foliage files were missing from the Xcode
  project since T5 landed; regenerated in the merge.
- T5 landed (caf5a42a0); merged code identical to T5's tested tree. It absorbs Doriax's foliage work and builds on T1's
  `edit_terrain`, `visualize_terrain` and delta undo. Next: T1's fan-out branch, then T3, then T2 and T6.
- T1 checkpoint landed in integration (e695a1bf3); the merged code is byte-identical to the tree T1 tested. T2, T3, T5 and
  T6 each merge integration once now. Landing order from here: T5, then T3, then T2 and T6 as they finish; each later lander
  merges integration again just before landing. T4 starts once T3 has landed.
- T5 finished (ebac93db7). Red controls caught: trees never drawn when their shaders compiled at runtime; Paint Trees disarmed
  in the Terrain window; a flat terrain placed no grass. Not run here: the prebuilt-player export test, the foliage web gate,
  Metal/D3D11/devices, eval cases. Its moving-camera p99 was over budget on a loaded host. It lands after T1's checkpoint, then
  merges integration once more to take T1's `editor::terraindetail` rename.
- Landing risk: `tools/check_fanout.py` caps how many units one header rebuilds at 150. T3's worst is 149 and T5 noted it sits at
  150 too, so merged streams may exceed it. Check it on every landing merge, and split includes if it fails.
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
