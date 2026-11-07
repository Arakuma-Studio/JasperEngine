# S74: Clip quality, animation visibility and IK accuracy

Wave: follow-ups to S21. Owner stream: animation. Status: implementing.

Source of the ideas: [ozz-animation](https://github.com/guillaumeblanc/ozz-animation) 0.17.0 (`744eb9d`,
MIT). S74 vendors nothing from it. Line numbers below are of `main` at `60cc1ba7c`.

## 1. Goal

Make animation in JasperEngine measurable, cheaper and more accurate, for humans and agents alike.

1. Anyone can read what a clip costs and how good it is, as the same numbers in the editor and through MCP.
2. Every project's clips use less memory with no loss, and sampling gets faster with no visible change.
3. The Animator's data race on the main-thread-only property writes is gone.
4. Import settings for a model's clips (Wrap Mode, Root Motion) exist in the editor, not only as JSON edits.
5. IK reaches its targets correctly, and a person can see why it did not.

The engine already has a Mecanim-style Animator, SIMD pose kernels, Foot IK and root motion, and these
stay. ozz is the source of a few better solver details, a key reduction algorithm and debug ideas.

## 2. Scope

### In scope

| Step | Content |
|---|---|
| 1 | Lossless clip storage, `memoryBytes()`, clip stats, the `Animation` profiler band and monitors, the property-write race |
| 2 | Benchmark rows on this repo's own build images, hot-path fixes, the tiered slerp |
| 3 | `ClipReport`, `ClipReduction`, `check_animation`, `optimize_animation`, Animation Statistics, `Reduce Keys...`, Root Motion foldout, overlay fixes, `--anims` checks |
| 4 | Model clip settings: `<model>.clips`, per-clip Wrap Mode and Root Motion, Loop Match, bulk actions, `edit_animation` support |
| 5 | IK debug (live table, overlays, `foot_sliding`, `transition`, `--anim-quality`), solver details, `animationEntities` default for new projects |

### Out of scope

- **Step 6, gated (see R50):** reduction inside the model cook, `Track` metadata slimming, the ghost preview.
  On JasperArcade-Ported (51 clips, 22 files, 1 mm, Python port with a modelled heap) the median key ratio
  was 1.1x and the median heap ratio 1.03x, against the 2x gate, and its clips total about 3 MB.
- Vendoring ozz or ACL, the `.ozz` format, ozz's key stream, 48-bit quaternions, half-float keys: they cannot
  hold events, eases, morph or property tracks, and `.ozz` rejects old versions, which fails "old projects must load".
- `MotionBlendingJob`, `TrackOptimizer`, CPU `SkinningJob`.
- Own specs, not here: shared GL bone texture, bone picking and Bone Inspector, profiler Animation module,
  Avatar Mask editor, clip curves driving parameters, clip Start/End and Loop Pose.
- Owned elsewhere: bone palette and `MeshComponent` size (`dc/mesh-memory`), the model cook and its inspector
  (S53), Animation Player clip speed and start offsets (`dc/runtime-core`).
- ARM measurements: the owner's decision is to test on this host only. Mobile numbers stay unmeasured, and docs say so.

## 3. Current state

Verified by reading unless marked as run.

- **Storage (run, on a patched copy):** `AnimationClip::buildNormalized` pushes tracks with no `reserve`
  (`engine/core/animation/AnimationClip.cpp:171-176,275`). On `walk_1mm` (132 tracks) 33,728 of 104,962 B of heap
  is `vector` slack. Each track also owns its time and easing arrays, even when identical or all linear.
- **Visibility:** the editor and MCP show key counts only. Nothing reports a clip's memory.
- **Profiler:** `standardCategory` maps only the exact name `"Animation"`, which no scope emits
  (`engine/core/profiler/Profiler.cpp:165-188`). Animator time lands in Scripts through `Update`, or Physics under
  `FixedUpdate`. `AnimatorSystem::Stats` is computed and never shown.
- **Race:** `writePose` runs in the parallel wave (`AnimatorSystem.cpp:424,687,692`) and writes `rig.properties`
  in two passes there (`:466-481`). Lua-field rows read the one Lua stack every frame even when unchanged
  (`PropertyBindings.cpp:1269`, `LuaBinding.cpp:1867-1878`). `active` and script `enabled` changes can run user Lua
  on a worker (`PropertyBindings.cpp:204-208,1301,1304`, `EntityRegistry.cpp:630-652`, `ScriptRuntime.cpp:325-336`).
  This breaks S21's "Lua never runs on workers". The Animation Player already defers to a main-thread flush
  (`ActionSystem.cpp:902,915-918`). Not reproduced yet.
- **Sampling:** `sampleQuaternion` calls `Quaternion::slerp`: one `acos`, three `sin`, 35 ns (run). `Quaternion::nlerp`'s
  `shortestPath` defaults to false where slerp's defaults to true (`Quaternion.cpp:576,627`).
- **Waste:** weight-0 layers are still sampled (`AnimatorInstance.cpp:1588-1606,1650,1657`). `CullUpdateTransforms`
  still raycasts for Foot IK (`AnimatorSystem.cpp:361-364,408-420`). `.anim` parses on the calling thread even in
  async mode (`AnimationClipPool.h:88-96`), at about 50-55 ms per MB (run). The Animation Player sets `needUpdate`
  unconditionally and allocates one vector per player per frame even when idle.
- **Model clips:** a dropped model gets a read-only table (`editor/window/InspectorWindowClips.cpp:1702`). Wrap is hard-coded
  to Once (`engine/core/animation/ModelSubClips.cpp:662-664`). `ClipRootMotion` has no UI. A Rig foldout exists
  (`editor/window/InspectorModelRig.cpp`).
- **IK (run on the real solvers):** two-bone hard-clamps at 0.999 of reach (`AnimatorIK.cpp:173`): the knee moves
  2.32 degrees per mm of target, then freezes. Look-at shares one turn over the chain and misses a near target by
  0.2-3.3 degrees on a neck chain at 1 m, and up to 8.8 degrees with a 0.5 m lever. Its test uses upright yaw, which cannot fail.
  On a slope the sole sinks 0.48 cm at 20 degrees and 1.07 cm at 30 degrees; at 45 degrees the default 30 degree clamp leaves the centre 2.07 cm under.
- **Humans cannot see IK:** agents get grounded, locked and root deltas; the editor shows nothing. The skeleton overlay
  reads Transforms, so it looks stale under `Write Bones: Exposed`.
- **Agents are blind** on clip memory, sampling cost, foot sliding and root drift. Finding a foot slide takes 65+ calls and
  the agent's own maths. Reducing memory with proof is not achievable.

## 4. Reference

Read-only, MIT. Study, re-implement in our style, credit in `NOTICE.md`.

| ozz file | Take |
|---|---|
| `src/animation/offline/animation_optimizer.cc`, `decimate.h` | Hierarchical error metric and Ramer-Douglas-Peucker decimation, with the changes in D5 |
| `src/animation/runtime/ik_two_bone_job.cc` | Soften, `mid_axis`, twist, `reached`. Its soften slope is 4/3 at onset despite its comment: use `1-(4/(a+4))^4` shaped for slope 1 |
| `src/animation/runtime/ik_aim_job.cc`, `samples/look_at/` | Chain walk with an aim offset. Line 202 skips the hemisphere fix-up when weighted: do not copy that bug |
| `samples/foot_ik/` | Slope-corrected ankle target only |
| `test/animation/runtime/ik_*_tests.cc` | Edge-case matrix (ZeroLength, Aligned, PoleTargetAlignment, ZeroScale) |
| `samples/optimize/`, `samples/two_bone_ik/`, `samples/motion_*` | Debug vocabulary: reached colours, rays, past and future paths |

## 5. Requirements

### Functional: step 1, facts and race

- **R1.** `AnimationClip::buildNormalized` reserves `tracks` to its final size.
- **R2.** Tracks with identical time arrays alias one range in the clip's storage. `Track::times` stays a pointer.
  `AnimatorRig`'s key-time grouping tests pointer equality before comparing contents.
- **R3.** A track whose segments are all linear has `easings == nullptr`. Every reader handles null, including
  `toData()` and `AnimationClip.cpp:326`, and `build(toData())` samples identically.
- **R4.** `AnimationClip::memoryBytes()` returns the heap the clip owns, measured from real capacities. It is the only source of a clip's memory number anywhere.
- **R5.** `AnimationClipPool` reports loaded clip bytes through `memory::TrackedBytes("Animation.Clips")`, visible in Jobs & Memory.
- **R6.** `AnimatorSystem`'s update and `ActionSystem`'s clip player pass emit scopes that `standardCategory` maps to `Animation`, so that band is non-empty when animation runs and its time leaves Scripts and Physics.
- **R7.** Monitors, read from `AnimatorSystem::Stats`: `Animators`, `Animators sampled`, `Animators culled`, `Anim Prepare ms`, `Anim Wave ms`, `Anim Finalize ms`, `Clip memory`.
- **R8.** Lua-field, script `enabled` and `gameobject.active` property rows are never read or written on a worker. They are applied on the main thread during the Animator's finalize, in the same order they sample, once per Animator. Plain component rows stay in the wave only where no two Animators can write one entity's row; otherwise they defer too. No Lua runs on a worker because of an Animator.
- **R9.** Crowd200 stays at 0 heap allocations in steady state after R8.

### Functional: step 2, measure and hot paths

- **R10.** `je-bench-anim` gains rows: real dense and reduced clips, key groups with different times, a retargeted crowd, an Animation Player crowd, rig space and skin matrices alone, `.anim` parse + build ms, clip heap bytes, and `Web30` (30 Animators, single thread).
- **R11.** The benchmark runs on this host through the linux image, and for wasm through the web image, with and without `-msimd128`. Results go in `docs/perf/animator.md` with the machine named, and state that ARM is unmeasured. New rows are information in `check_bench.py` unless a budget below names them.
- **R12.** `sampleQuaternion` uses a tiered slerp: plain normalised lerp at `|d| >= 0.999`, corrected normalised lerp down to `|d| >= 0.9659` (30 degrees between keys), exact `Quaternion::slerp` beyond. Correction: `t' = t + k t (t - 0.5)(t - 1)`, `k = (2/3) x (1 + x/6)`, `x = 1 - |d|`. The change is in `sampleQuaternion`, never in `Quaternion::slerp`, and it takes the shortest arc. It returns exact keys at `t = 0` and `t = 1`. A comment records the Taylor derivation of the reparameterisation `sin(t th) / (sin(t th) + sin((1-t) th))`.
- **R13.** Rotation error of R12 against true slerp is at most 0.01 mm at 1 m radius for segments up to 30 degrees.
- **R14.** Weight-0 layers are not sampled, except where an interruption snapshot or a later weight change in the same frame needs the pose.
- **R15.** Foot IK does not raycast under `CullUpdateTransforms` while culled.
- **R16.** `.anim` parses on a background thread where threads exist, through `pollOrLoad`, and synchronously otherwise (web without threads). The first-use result is the same on both paths.
- **R17.** The Animation Player compares before setting `needUpdate`, allocates nothing per frame when idle, and uses pre-resolved `rest` where the layered path already has it.

### Functional: step 3, clip quality

- **R18.** `editor/animation/ClipReport.*`: `anim::ClipReport analyzeClip(const ClipReportRequest&)` computes everything in the field table of D3. Panels and tools print its struct; neither computes a number.
- **R19.** `measureClipError` samples both clips with the runtime's `sampleTrack`, runs `poseToRigSpace`, and compares joints and virtual points at `skin_distance_mm`. After a write it re-reads the output from disk. The reducer has no reporting path.
- **R20.** `editor/animation/ClipReduction.*` implements D5. Output keys are a strict subset of source keys, in order, with values byte-identical.
- **R21.** Reduction keeps clip length and every event, leaves non-eligible tracks byte-identical, and is deterministic at any thread count.
- **R22.** `tolerance_mm` bounds the measured max error at `skin_distance_mm`. Bisect the internal per-track tolerance (at most 8 steps) until the measured max is within it. Effective limit: `min(tolerance_mm, 0.1 % of rig height)`.
- **R23.** Feet are tightened by default: bones the humanoid map or foot names identify as feet and toes use half the clip's tolerance, because a planted foot's path can grow from 6.9 to 30.2 mm with joint error under 1 mm. `joints` overrides apply, and the up-chain `min` is documented.
- **R24.** A reduction's output is a new `.anim` by default, recording `optimize: {tolerance_mm, skin_distance_mm, source_sha1, max_error_mm}`. The key is added to `isClipExtensionKey` so it does not warn. Only an explicit `overwrite` or `Replace This Clip` changes the source, as one undo step. `model#clip` input writes a new `.anim`; a model file is never changed.
- **R25.** `check --anims` gains `anim-redundant-keys`, `anim-constant-track`, `anim-rotation-flip`, `anim-optimize-stale`. All are warnings.
- **R26.** `Write Bones: Exposed` overlay fix: the skeleton overlay reads `instance.pose()` when bound. Under Transform the Inspector shows `Not written to this GameObject (Write Bones: Exposed)`. The root marker draws when the parent is not a bone.

### Functional: step 4, model clip settings

- **R27.** Per-clip settings live in the sidecar `<model>.clips` (D4). Absent file or absent clip means today's values, byte for byte.
- **R28.** `ModelSubClips` applies Wrap Mode and Root Motion from the sidecar at `ModelSubClips.cpp:662-664`. Play and an exported game read the same file, so they match, and no cook is involved.
- **R29.** The sidecar hot-reloads through the `.humanoid` wiring (`editor/reload/handlers/Animations.cpp:196`, `reload/handlers/Common.cpp:36`, `editor/mcp/McpAutoSync.cpp:171`) and ships in exports with no extension allow-list change (`Exporter.cpp:2804,3037`).
- **R30.** Loop suggestion: a clip with no setting whose name suggests a cycle (`loop|idle|walk|run|cycle`) and whose Loop Match is under 10 mm shows `Suggestion: looks like a loop [Use]`. Nothing changes until the user clicks. No automatic Loop, ever.
- **R31.** Bulk: multi-select with mixed values shown as a dash, `Extract to .anim...` with one folder prompt, `Add to Animator...` making one state per clip in one undo step.

### Functional: step 5, IK

- **R32.** Two-bone: `soften` (0 off, default 0, slope 1 at onset), `hingeAxis` (default auto, today's choice), `twist` (default 0 degrees), `reached` output. A soft-limited solve keeps the knee moving continuously.
- **R33.** Look-at walks the chain, base to tip, so each joint re-aims from its current pose. `aimOffset` is an offset from the last joint to the eyes (default none). The aim error at the end, with clamp off and weights 1, is at most 0.1 degree on a forward-leaning chain at 1 m. Keep the engine's weights and clamp.
- **R34.** Foot IK corrects the ankle target for the ground slope, so the sole stays on the plane at up to the clamp angle, within 1 mm at 30 degrees.
- **R35.** R33 and R34 apply to existing scenes without opt-in (owner decision). No stored field changes meaning, so there is no version bump, and release notes in `docs/upgrading-from-main.md` say so. `soften` stays off by default, because it changes foot behaviour.
- **R36.** `AnimatorDebugSnapshot makeSnapshot(instance, component)` in `editor/animation/AnimatorDebug.*` holds IK runs in world space (weight, reached, reach error, hit, normal, pelvis offset, lock drift) and root speed, yaw and slide. It backs the live table, overlays and tools. Metres convert to mm only inside it.
- **R37.** Foot sliding: with foot goals or an explicit `feet`, `foot_sliding` reports max slide in contact. On generic rigs with neither, it reports `contact_source: "none"`, not a guess.
- **R38.** New projects: `Project Settings > Animation > Animation Entities: Auto` makes a model with 5 or more clips load with `animationEntities = false`. Missing key means today's behaviour, so old projects do not change. The struct default at `ModelComponent.h:72` does not change.

### Platform and backends

- **R39.** Everything runtime-side is CPU only and portable C++20 within `tools/check-cxx-subset.py`. No compute, SSBO, bindless or indirect draws. It builds for linux, web and android here; Windows, macOS and iOS stay correct by following existing patterns.
- **R40.** A disabled feature costs nothing. Overlays off: no draws and no snapshot. No `<model>.clips`: one failed lookup per model load, cached. Reduction, reports, overlays and `ClipReduction` compile out of players.

### Performance

- **R41.** Crowd200 stays within S21's budgets (p95 at most 2.5 ms, serial at most 8.8 ms, 0 allocations) on server-comparable runs; on this host, against a baseline run of the same machine, it does not get slower beyond noise.
- **R42.** Clip heap: `walk`-shape reduced clip (132 tracks, 911 keys) at most 72,000 B; dense `pab_walk` shape (201 tracks, 8,241 keys) at most 190,000 B. These are test shapes generated in the test, not ozz assets.
- **R43.** Tiered slerp: the 65-bone sample micro gets at least 1.5x faster than exact slerp on this host.
- **R44.** Owner budgets, recorded in `docs/perf/animator.md`: `Web30` under 4 ms single thread, clip memory under 16 MB. A miss is reported, with the row's number, not hidden. A native-only result is labelled as such.

### Editor UX (human)

- **R45.** `.anim` Inspector: info line (`1.20 s · 30 Hz · 65 tracks · 2,340 keys · Memory 31 KB · File 112 KB · 9 constant tracks`), Root Motion foldout (`Root Motion Node`, `Root Transform Rotation`, `Root Transform Position (Y)`, `Root Transform Position (XZ)`, `Bake Into Pose`), each one undo step through `writeClipRootMotion` and `document.commit`, and `Reduce Keys...`.
- **R46.** `Reduce Keys...` dialog: `Error Tolerance` (mm), `Skin Distance`, before and after Memory and Keys, `Max Error`, `Mean Error`, `Worst Joint`, output path, `Replace This Clip`. Preview writes nothing. The selected clip's numbers come from R18.
- **R47.** Animation Statistics, under `Window > Analysis`: scan project or open scenes, totals, columns Clip, Source, Length, Tracks, Keys, Memory, Max Error, Users, Loaded; filters All, Loaded, Unused, Over budget, with an editable per-clip budget. Double-click selects. No edits. `Unused` warns that scripts can name clips.
- **R48.** Model clips section in the Inspector, replacing the read-only table at `InspectorWindowClips.cpp:1702` and shaped like `TextureImporterView`: working copy, Apply, Revert, one undo step. Columns Clip, Length, Wrap Mode, Root Motion, Memory, Keys, Loop Match. It must be movable into S53's Animation tab without rewriting.
- **R49.** IK debug: `Gizmos > Animation` toggles Root Motion, IK, Foot Trails (all off). Scene colours: target green when reached and red when not, chain cyan, pole diamond with a dashed line to the mid joint, ray yellow with hit green and miss white, locked foot orange, foot trail green when planted and yellow to red by slide speed, root motion past red and future green. Animator Inspector live table: Weight, Reach Error, State, plus Root Motion speed, yaw, foot speed and slide warning.

### Step 6 gate

- **R50.** Reduction in the cook, `Track` slimming and the ghost preview are built only if a project shows at least 2x median clip heap savings at 1 mm through the real importer, on at least 20 clips from at least 3 sources, after the lossless work in step 1. S74 ships `check_animation` and the benchmark that can show this.

### Agent UX

- **R51.** New tool `check_animation`, group `animation`, per D6.
- **R52.** New tool `optimize_animation`, group `animation`, per D6. `optimize` joins `VERBS` in `tools/mcp/tests/test_tool_conventions.py` with a comment.
- **R53.** Extensions:
  - `get_animation`, `get_asset_info` (`.anim`): `stats`.
  - `get_asset_info` (model): `animations[i]` gains `keys, runtime_bytes, wrap, root_motion`; `skeleton: true` returns `skeleton_summary {joint_count, skinned_joints, max_depth, roots}`.
  - `get_model_info`: `bones[i].parent`.
  - `sample_animation_pose`: accepts an Animation Player; `times` (at most 64) and `space` (world, root, local), ascending in one pass.
  - `get_animator`, `control_animator` `live`: `ik[i]` gains `weight, reached, reach_error_mm, hit_point, hit_normal, pelvis_offset_mm, lock_drift_mm`; `root_motion` gains `speed_mps, yaw_deg_s, accumulated_position, accumulated_yaw, slide_mm_s`.
  - `set_scene_camera {display}`: `show_ik, show_root_motion, show_foot_trails`.
  - `get_frame_times {monitors}`: the R7 names.
  - `edit_animation`: on `model#clip`, `wrap` and `root_motion` write `<model>.clips`, one undo step. A model's other fields keep today's errors.
- **R54.** `check --anim-quality` runs `anim-bone-limit`, `anim-loop-seam` (over 10 mm), `anim-root-drift` (over 20 mm in place), `anim-foot-slide` (over 15 mm in contact). It loads models, not scenes, is not in `--hook`, and joins `--all`. All are warnings, and exit codes keep their meaning.
- **R55.** Budget: `tools/mcp/budget.json` gets `groups.animation` for 13 tools; each tool within 1,500 chars; `tool_budget.py --check` passes. No tool moves to core.
- **R56.** Guidance goes in a "Quality: numbers before pictures" section of `docs/mcp/guide/animation.md`. The agent guide (`McpInstructions.cpp`) is untouched unless a `tools/mcp/eval` case fails without a line.
- **R57.** Lua: `Animator:isIKReached(constraint)`, plus the new constraint fields wherever constraints are reachable. Nothing else, since the rest is editor-time. Stubs regenerate through `tools/lua_api`.

### Data and compatibility

- **R58.** Old projects load and play the same, except R12's rotation change within R13 and the fixes in R35. `.anim` v1 and v2 stay byte-stable, `writeClip(parseClip(x)) == x`, and nothing writes `.anim` except an explicit command.
- **R59.** All new persistent writes go through `AtomicFile` or a helper built on it. `tools/check_raw_writes.py` and `tools/check_stream_keys.py` pass.
- **R60.** `tools/test-projects/upgrade-from-main` and `test_upgrade_from_main.py` stay green.

## 6. Design decisions

- **D1. No vendoring.** Three libraries for about 146 core lines that need changing anyway. The verdict was measured on x64, so it can be reopened if a project misses a budget on one target and an out-of-tree harness shows at least 2x there.
- **D2. One producer per number.** `AnimationClip::memoryBytes()`, `analyzeClip`, `makeSnapshot`, `profiler::monitors()`. A panel and a tool may format, never calculate.
- **D3. One field table** (`kClipReportFields[] = {key, label, unit, format}`) drives Inspector columns and JSON keys. A gtest checks that every table key appears in the report JSON and the reverse. mm everywhere, never cm.

| Label | Key | Unit |
|---|---|---|
| Memory | `runtime_bytes` | bytes |
| File | `file_bytes` | bytes |
| Keys, Tracks | `keys`, `tracks` | count |
| Error Tolerance | `tolerance_mm` | mm |
| Skin Distance | `skin_distance_mm` | mm |
| Max Error, Mean Error | `max_error_mm`, `mean_error_mm` | mm |
| Worst Joint, at time | `worst_joint`, `worst_time_s` | name, s |
| Loop Match | `loop_seam.max_error_mm` | mm |
| Average (Root Transform) | `average_speed_mps`, `average_yaw_deg_s` | m/s, deg/s |
| Reach Error | `reach_error_mm` | mm |
| Pelvis Offset, Lock Drift | `pelvis_offset_mm`, `lock_drift_mm` | mm |
| Slide | `slide_mm_s` | mm/s |

- **D4. `<model>.clips` is a sidecar like `.humanoid`,** not `.meta` settings. `.meta` does not ship, and a cooked copy is a cache: a fresh clone or a failed cook would play `Walk` as Once. One file read the same way by the editor and players keeps Play equal to export. JSON, `{"version": 1, "clips": {"Walk": {"wrap": "loop", "root_motion": {"bone": "", "bake_rotation": false, "bake_position_y": true, "bake_position_xz": false}}}}`. Unknown keys survive a write. It is a runtime setting, so S53's cook does not change it and it does not change S53's cook. The agreement with S53 is only the Inspector placement (R48).
- **D5. The reducer differs from ozz's on purpose:**

| ozz does | We do | Why |
|---|---|---|
| float32, `1 - c²` on raw keys | Double, rotations renormalised first | Otherwise errors under about 0.7 mm per metre read as 0 |
| Tolerance per track, errors stack | Bisect until measured error is within tolerance | The setting means what it says |
| nlerp | The engine's sampler | Otherwise the metric judges something else |
| Empty track means identity; drops trailing keys | Constant track becomes 1 key. Never remove a track. A moving track keeps first and last key | No track means rest here, and any `loop` track's period depends on its last key |
| Lengths from animated T keys | Fall back to rest translation | glTF often has no T on bones |
| `distance` free | `skin_distance_mm` default 100, floor 10 | Distance 0 let a vertex move 201 mm |
| Metres assumed | R22's effective limit | A tiny model must not get a body-sized error |
| All keys linear | STEP-only tracks kept whole; tangents, mixed eases, morph, lattice, property, humanoid channels skipped and counted (`skipped_tracks`) | No silent quality change. No resampling |
| NaN passes | Reject non-finite with a named error | |

- **D6. Tool shapes.**
  - `check_animation`: `readOnly: true`, `allowedWhilePlaying: true`, job, cancellable, writes nothing. Parameters: `clips` (`.anim`, `model#Clip`, a model means all, empty means project), `entity`, `scene`, `state`, `checks` (`stats|loop_seam|root_motion|foot_sliding|transition`), `compare_to` (a clip), `transition {from, to, duration, at}`, `feet`, `move_speed`, `record_seconds`, `limits {error_mm, foot_slide_mm, root_drift_mm, pop_mm}`, `top`, `wait`, `timeout_seconds`. Returns `{ok, rig, clips[], totals, findings[]}`. `findings` use the `ProjectChecks` shape (`docs/mcp/checks.md`). `ok: false` is a successful call.
  - `optimize_animation`: `readOnly: false`, `idempotent: true` with `overwrite`, `ToolActivity::Edit`, `allowedWhilePlaying: false`, job, the shape of `bake_vertex_animation`: without `output` it writes nothing. Parameters: `clips` (required), `on`, `scene`, `tolerance_mm` (default 1, 0.01-100), `skin_distance_mm` (default 100, floor 10), `joints [{path, tolerance_mm}]`, `output` (bool or path), `overwrite`, `root`, `wait`, `timeout_seconds`. Returns `{written, tolerance_mm, effective_tolerance_mm, skin_distance_mm, total_bytes_before, total_bytes_after, clips[], warnings[], undo}`.
  - `bytes_before` and `bytes_after` are `runtime_bytes` of the two clips, named in the field table with one label, so D3's rule holds.
- **D7. The race fix copies the Player's flush.** Deferred rows are stored in per-Animator buffers the wave fills and finalize drains, preallocated at bind, so R9 holds.
- **D8. Test on this host.** `tools/check-changed` per commit, then the full suite before landing. `MCP_SUITE=all` skips the fixture-only tests here unless a fixture exists, and reports say which.

## 7. Acceptance criteria

| # | Check | Verification |
|---|---|---|
| AC1 | R1-R4, R42 | `ClipMemoryTest.*`: byte bounds red on `main`, `build(toData())` identical with shared times and null easings |
| AC2 | R5-R7 | `ProfilerAnimationTest`: an Animator scene gives `categoryMs[Animation] > 0`, red on `main`. MCP: `get_frame_times {monitors}` returns the R7 names |
| AC3 | R8, R9 | `AnimatorPropertyThreadTest`: 5+ Animators keying a Lua field, `active` and `enabled`, on `SANITIZE=thread`; red on `main` (report the TSan output), green after. A test records the thread of each write. `PropertyTracksPlayOnTheAnimator` stays green; Crowd200 0 allocations |
| AC4 | R10, R11, R44 | `je-bench-anim --json` has the new rows; `docs/perf/animator.md` holds the table for linux and wasm, with ARM declared unmeasured |
| AC5 | R12, R13, R43 | `KeyframeSampling.TieredSlerpMatchesSlerp`: bound per segment angle, tier boundary, end keys, shortest arc. Only `ValueSamplersMatchTheFormerActionSystemExpressions`'s reference changes. `MatchesThePlayer`, `ParallelEqualsSerial`, `Layers.MixingTransformsLimitAState`, `Layers.LayersWriteChannelsIndependently` stay green unedited. If another test needs an edit, keep exact slerp and record it |
| AC6 | R14, R15 | `AnimatorInstance` test: a weight-0 layer's sampler is not called, red on `main`; interruption snapshot stays correct. Foot IK culled: raycast count 0, red on `main` |
| AC7 | R16 | Pool test: async load returns from a worker, same bytes as sync; sync path on no-thread builds |
| AC8 | R17 | Animation Player idle: 0 allocations over 600 frames, red on `main`; no `needUpdate` when the value is unchanged |
| AC9 | R18-R22, R24 | `ClipReductionTest`: subset invariant, length kept, events byte-identical, determinism, NaN named error, float32-noise fixture red without renormalising, bound holds on a planted curve set. `ClipReportTest`: field table equals JSON keys; a planted 5 mm bone move makes `compare` fail |
| AC10 | R23 | A foot on a planted curve gets half the tolerance; a `joints` override wins |
| AC11 | R25, R54 | `test_animation_quality.py`: planted clips trigger each code, and a clean clip triggers none. Exit codes unchanged |
| AC12 | R26 | Exposed scene: drawn overlay bone position equals the rig pose, red on `main` |
| AC13 | R27-R29 | MCP + unit: setting Loop on `model#Walk` plays looping in Play and in a Linux export, red before; no sidecar means identical output; undo restores bytes; sidecar edit hot-reloads |
| AC14 | R30, R31, R48 | UI test: suggestion shows and changes nothing until used; `find_ui_item` row values equal `get_asset_info`; bulk Apply is one undo step; screenshots from an isolated editor |
| AC15 | R32-R34 | `AnimatorIKTest` red first per fix: near-straight knee continuous with soften, `reached` correct, forward-leaning chain within 0.1 degree, slope sole within 1 mm at 30 degrees. The ozz edge-case matrix passes. S21 AC5 stays green |
| AC16 | R35, R38, R58, R60 | Upgrade check green. Old project keeps `animationEntities` behaviour; a new project with a 5-clip model gets `false` |
| AC17 | R36, R37, R49, R40 | Snapshot unit test; `find_ui_item` row equals tool value. Overlays off: no line draws (counter). Overlay screenshot. Planted 50 mm slide fails, a clean clip passes, and a generic rig with no feet reports `contact_source: "none"` |
| AC18 | R45-R47 | UI tests: info line values equal `get_animation.stats`; foldout undo; Reduce Keys preview writes nothing, and its output passes `check_animation compare_to`. Statistics rows equal tool values |
| AC19 | R51-R53, R55 | `test_animation_quality.py`: group hidden until `tool_groups`, tools listed after, jobs complete, preview then write. Journey: reduce and prove in 3 calls. `tool_budget.py --check` passes. `docs/mcp/TOOLS.md` regenerated |
| AC20 | R57 | Lua test calls `isIKReached`; `tools/lua_api` output contains it |
| AC21 | R39, R41, R59 | `tools/build.sh linux`, `unit-tests`, `web`, `android` with `WERROR=1`; `check-cxx-subset`; raw writes and stream key lints; Crowd200 run against the same-machine baseline |
| AC22 | Whole | Before landing: `WERROR=1 tools/build.sh linux`, `WERROR=1 tools/build.sh unit-tests`, `MCP_SUITE=all JASPER_SLOW_BUILD=1 JASPER_SLOW_TESTS=1 tools/build.sh mcp-tests -j <n>`; `SMOKE_METHOD=compile tools/smoke-test.sh` |

## 8. Dependencies and conflicts

- **Builds on:** S21, S15, S14's `TextureImporterView` pattern, `bake_vertex_animation`'s tool shape.
- **S53** (`feat/s41-importer`, in progress): owns the cook, `import.modelImporter` and the Model Import inspector. Whichever lands second moves R48's section into the Animation tab. S74 adds no cook.
- **`dc/mesh-memory`** (in progress): owns the bone palette. S74 does not touch `MeshComponent`.
- **`dc/runtime-core`** (in progress): touches `ActionSystem` for clip speed and start offsets, so R17 stays small. Merge hotspot.
- **`dc/agent-authoring`:** the only program restructuring the guide (R56).
- **Merge hotspots:** `editor/mcp/McpToolGroups.cpp`, `tools/mcp/budget.json`, `docs/mcp/TOOLS.md`, `docs/specs/README.md`, `editor/window/InspectorWindowClips.cpp`, `engine/core/subsystem/AnimatorSystem.cpp`, `engine/core/animation/AnimatorInstance.cpp`.

## 9. Risks and defaults

| Risk | Default |
|---|---|
| TSan stays silent on `main` for AC3 | Find out why before changing code, since reading says it must fire. `JASPER_LUA_THREAD_CHECK` only hooks the allocator, so do not rely on it |
| R12's tier does not reach R43 or needs other test edits | Keep exact slerp and record a deviation |
| Foot contact on generic rigs is unreliable | R37: report only with foot goals or `feet` |
| Thresholds in R30 and R54 are assumed | Calibrate against JasperArcade-Ported and Quaternius, kept outside the repo; record the result |
| `Web30` cannot run in the web image under `node` | Run through `tools/web/browser_gate.py`, or record a deviation naming what was measured |
| Soften looks wrong on feet | Off by default (R35) |
| Fixture-only MCP tests skip on this host | D8: list them in the report |

## Deviations

- **R7, `Clip memory`:** read from `AnimationClipPool::loadedBytes()`, the R5 count, not from `AnimatorSystem::Stats`.
  Stats are per scene and the pool is process-wide, so a sum over scenes would multiply it, and a scene with only Animation
  Players has no Animator run to fill it. The other six are read from `Stats`, through `takeFrameStats()`: a frame's fixed
  steps and its update, which `getStats()` (the last run only) cannot give.
- **R3, the last key's ease:** a key's `ease` belongs to the segment it starts, so the last key's starts none, but a file can
  hold one and `.anim` must stay byte-stable through `toData()`. `Track::trailingEase` keeps it. R3's rule itself holds: a
  track with only that ease has `easings == nullptr`.
- **R53, `get_asset_info` on `.anim`:** `stats` is under `clip`, with the rest of that tool's clip fields.

Step 2 (stream `hot`):

- **R10, clip heap bytes.** The `*_heap_bytes` rows read `AnimationClip::memoryBytes()` (R4), the one producer of that number.
- **R10, real clips.** Real clips cannot be committed, so `--clip <file.anim>` adds them as `real_*` rows on the machine that has them; the always-present dense and reduced rows are R42's generated shapes. The reduced real clips are an earlier Python port's 1 mm output, not `ClipReduction` (step 3).
- **R11, wasm.** Measured under node (V8) in the web image, one thread, not in a browser (the web gate's Chrome was not used). There is no run of the old sampler on wasm, so R43 is a native result. `-msimd128` covers the pose kernels only, as in shipped web builds; the sampler is scalar wasm in both variants.
- **R14.** A layer of weight 0 is still sampled while a transition or montage runs on it: an interruption can freeze that pose at any time. One known difference from sampling always: an interruption with no output (two transitions started before an evaluation, weight 0, no transition before them) samples the last skipped state with the parameters of the next evaluation, so a blend tree whose parameters changed in that one frame starts from slightly different leaf weights. It shows only if the weight also comes up during that blend.
- **R14, root motion.** A layer of weight 0 no longer samples its root, so with no other layer giving root motion `output.hasRootMotion` is false where it was true with a zero delta. Nothing moves differently.
- **R16, small files.** A `.anim` under 64 KB parses in the call even where threads exist. A job starts a clip at least one frame late, a varying number of frames under load, and `test_upgrade_from_main.py` (R60) failed on exactly that with a 526 B clip: its beacon's timeline moved one frame against the rest of the transcript. Under 64 KB the parse is a few milliseconds (about 50 ms per MB). A larger clip's first play can still start frames later than on main, its own timeline intact: that is R16's trade, and the upgrade fixture has no such clip.
- **R16.** The worker path applies in async loading (`Engine::isAsyncLoading`, on in the editor and exported games), as for sub-clips. Without it, `.anim` still loads in the call. `AnimatorSystem::preview` and `AnimationPlayer::sample` load a `.anim` in the call in every mode, through `pollOrLoad(..., animInCall)`, because their callers read the pose when they return. An Animator's first bind now waits for all its clips, for sub-clips too, so time starts as on the synchronous path; this also changes async sub-clip loads, which used to start the states' time at the first bind. AC5's limit on test edits is about the tiered slerp and is unaffected: only `ValueSamplersMatchTheFormerActionSystemExpressions`'s reference changed for that.
- **R43 and full clips.** R43 holds on its micro (1.86 x on the merged tree). Full clips gain -2 % to -17 % in sampling and a segment over 30 degrees costs about 25 % more per call: `docs/perf/animator.md` has both.
