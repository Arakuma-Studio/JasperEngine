# S21 — Animation (Animator Controller)

Wave 2. Owner stream: animation. Status: re-scoped (see §0), implementing.

## 0. Re-scope

The first version of this spec (649 lines, kept in git history at `3a8a5734e`) was written before
upstream landed its own animation systems in `oe-port`. Upstream now provides:

- **Animation Clips** (`docs/animation-clips.md`): `.anim` JSON clip assets with a GUID, tracks bound
  by child path (`engine/core/animation/`: `AnimationClip`, `AnimationClipIO`, `ClipBinding`,
  `KeyframeSampling`, `ModelSubClips`, `pool/AnimationClipPool`), model sub-clips
  (`model.gltf#Walk`), event crossing (`collectCrossedEvents`: forward, backward, loop and ping-pong
  wraps, once per crossing), and the **Animation Player** (Unity's legacy Animation component:
  clip list, play/crossFade/blend/queue, wrap modes, Lua events). Editor: the Animation window's
  clip mode (dopesheet, curves view, events lane, Record), Inspector drawers, glTF/FBX clip
  extraction, Convert/Bake; MCP `edit_animation`, `get_animation`, `preview_animation`.
- **Pose Animation** (`docs/pose-animation.md`): `.poseanim`, `.humanoid` sidecars, 22 humanoid
  slots (`pose/HumanBones.h`), name auto-detection (`pose/HumanoidMap`), skeleton-independent
  joint angles (`pose/PoseRig`: `anglesFromLocal` / `localFromAngles`), analytic two-bone IK
  (`pose/PoseIK`), the Pose Animator; MCP `get/edit/render_pose_animation` (group `animation`,
  `set_humanoid` op writes `.humanoid`).
- Vertex Animation Textures arrive with upstream round 3 (unrelated to this stream's runtime).

What changes in S21:

| Original S21 item | Now |
|---|---|
| New clip format, sampler, YAML `.anim`, binary cook (R2, R31, R34, D13) | **Dropped.** The Animator plays upstream's `.anim` and `model#clip` clips through `AnimationClipPool` and `KeyframeSampling`. Assets are JSON like `.anim` (the engine runtime has nlohmann JSON, not YAML), shipped as files like `.anim`; no cooked binary. |
| Events port from OpenEngine (R8 crossing logic) | **Reuse** upstream's `collectCrossedEvents` and `AnimationEvent`; S21 adds the controller rules (leader-only per layer, montage events, once per Animator). |
| Simple mode / legacy upgrade (R12, R33) | **Dropped.** The Animation Player is the simple path and upstream's "Use Animation Player" is the legacy-entity upgrade. S21 adds **Create Animator Controller** from an Animation Player. |
| Humanoid: VRM/KHR importers, OpenEngine name matcher, own `HumanBone` (R11) | **Reuse** upstream's 22-slot humanoid, auto-detect and `.humanoid` sidecar. Retargeting goes through `PoseRig`'s joint-angle frames (D12). No VRM/KHR extension reading. |
| Dope sheet / curve editor as a new widget (R27, D15) | **Extend** upstream's Animation window: free tangent editing with Unity tangent modes in the curves view, controller clips in the clip dropdown, Animator-driven preview. `math/Curve.h` (S20) stays the particle curve. |
| 2D generic rig, sprite-frame tracks, property curves | **Out of scope** (upstream reserves the `property` channel; sprites keep Sprite Animations). The Animator does work on any hierarchy (cut-out 2D rigs made of entities), because it binds by path like upstream clips. |
| Skin matrices in the wave (R22) | Kept: the Animator writes `bonesMatrix` of the skins under it; RenderSystem skips those bindings. |

The Animator **builds on** upstream: same clips, same path binding grammar, same rest-value rule,
same events, same humanoid data. The decision on how the two players relate (D1):

> The **Animator** is the controller layer above clip sampling (state machines, blend trees,
> layers, masks, montages, IK, root motion). The **Animation Player** stays the simple path (a
> list of clips with play/crossFade/queue). Both play the same clip files. On one hierarchy, the
> Animator runs after the players and owns the bones it binds (it overwrites what a player wrote
> to them this frame); the Inspector warns when both are on one root.

## 1. Goal

Gameplay characters driven by a Mecanim-style **Animator Controller**: parameters, layered state
machines, blend trees and transitions with conditions; avatar masks and additive layers; montages
on slots; an IK constraint stack; root motion; and animation events, all integrated in one
evaluation. Humanoid clips retarget between humanoid models. Editors: the Animator window
(state graph with live debugging), blend tree, avatar mask and montage editors, tangent editing in
the curves view. Lua and MCP drive it all. 200 characters per frame on every platform (SoA pose,
SIMD, the S06 job system). Old scenes play exactly as before.

## 2. Scope

**In scope**
- Engine `engine/core/animation/` (namespace `jasperengine::anim`): `AnimatorRig` (bones bound by
  path), SoA `Pose` and SIMD `PoseOps`, `RigClipSampler` (upstream clips onto a rig),
  `ControllerDef` + `ControllerIO` (`.controller`), `BlendTree` weights, `AvatarMask` + IO
  (`.mask`), `Montage` + IO (`.montage`), `AnimatorInstance` (state machines, transitions,
  layers, montage slots, events, root motion), `AnimatorIK` (TwoBone, FABRIK, LookAt, FootIK,
  FootLock), `Retarget` (humanoid), `AnimatorAssetPool`.
- `.anim` `root_motion` block (optional key, format version unchanged).
- `component/AnimatorComponent.h`, `subsystem/AnimatorSystem.{h,cpp}`, `object/Animator.{h,cpp}`
  (Lua/C++ wrapper), Lua bindings, Factory codegen, S08 reflection tables.
- Editor: Animator window, state/transition/blend-tree Inspector, `.controller`/`.mask`/`.montage`
  asset Inspectors (mask and montage editors), Animator component drawer (avatar status,
  Configure Avatar through the upstream bone-mapping dialog), Create Animator Controller from an
  Animation Player, curves-view tangent editing, Animator edit-mode preview with particle-aware
  scrubbing (S20 seek), commands and shortcuts (S25), settings (S25 SettingsRegistry).
- MCP tools (group `animation`), `docs/mcp/guide/animator.md`, user doc `docs/animator.md`,
  Lua docs, Unity glossary rows.
- Tests (gtest, MCP, goldens), E benchmarks.

**Out of scope**
- GPU/compute animation sampling (README: no compute requirement).
- Humanoid muscle space (mirroring arrived with upstream's anim-retarget, §12), sync layers, Unity `.controller`/`.anim` import (README: no
  Unity importer), ragdolls, clip compression, property/sprite-frame curves, VRM/KHR humanoid
  extensions, rate LOD by screen height.
- OpenEngine's graph runtime, editor panels, retarget nodes (README do-not-copy: their editor UI
  toolkit; original §4 verdicts stand). No OpenEngine file is copied.
- Changing the Animation Player, Pose Animator or entity Animation behaviour.

## 3. Current state (oe-port `3a8a5734e`)

- Clips: `ClipData` / immutable `AnimationClip` (per-track typed arrays), channels Position,
  Rotation (x y z w), Scale, Morph, Lattice, Property (reserved). Paths relative to the playing
  root (`ClipBinding.h`: `Name#N` siblings). `KeyframeSampling.h`: `locateKey` with a cursor
  hint, `sampleVector3`, `sampleQuaternion`, `sampleFloatRow`, `wrapClipTime`, `trackTime`,
  `collectCrossedEvents`. `AnimationClipPool`: `pollOrLoad`, generations, `revision()`.
- Animation Player: `ActionSystemPlayers.cpp` accumulates weighted samples into
  `std::unordered_map<Entity, …>` blends (`TransformBlendAccum`) on the main thread and flushes
  them with `flushPoseBlend`; rest values per (entity, channel) taken before the first write.
- Pose: `pose::Skeleton`, `HumanoidMap` (22 slots), `PoseRig` (slot frames `left`/`neutral`,
  `humanScale`, `characterRotation`), `PoseRigCache`, `.humanoid` IO, `PoseIK` (two-bone, pole).
- Skinning: `RenderSystem.cpp` after `propagateTransforms` recomputes `bonesMatrix` per
  `ModelSkinBinding` from joint entities' `modelMatrix` (one `findComponent` per joint).
- S22 left `PhysicsSystem::setRootMotion(entity, worldVelocity, yawRate)` for "S21's Animator".
- No state machine, blend tree, layer, mask, additive, montage, root motion or IK stack exists.

## 4. Reference (OpenEngine, MIT)

Unchanged verdicts from the first version (field checklists only: their state machine lacks exit
time, any-state and interruption; their 2D blend space reads only 16 samples; their root motion
uses the blended pose's frame delta; their montage is a stub). Nothing is copied, so `NOTICE.md`
gets no entry from this stream.

## 5. Requirements

### Functional — runtime
- **R1 Rig.** An Animator binds a rig on its entity's hierarchy: every entity a track of any clip
  of its controller (and montages, overrides) resolves to by path, plus the humanoid slot bones,
  plus their ancestors below the root, sorted parents-first. Each bone keeps its rest local TRS,
  taken from the scene before the Animator first writes it (upstream's rest rule). Morph weights
  and lattice offsets are float channels of the rig. A rig is rebuilt when the hierarchy revision
  or a clip generation changes; state is kept.
- **R2 Clips.** States, blend-tree children and montages reference upstream clips (`.anim` or
  `model#clip`) as `{path, guid}`. Sampling uses `KeyframeSampling` with per-instance cursors
  (allocation-free). Relative tracks combine with rest exactly as the Animation Player does. A
  missing or failing clip contributes nothing and is reported once.
- **R3 Pose ops.** SoA, SIMD (S20 `SimdFloat4.h`; Scalar always built): blend two, accumulate N
  with hemisphere alignment and nlerp normalisation, masked override, masked additive, local to
  model space with scale.
- **R4 Controller.** `.controller` asset: parameters (Float, Int, Bool, Trigger with defaults),
  layers (name, weight, Override/Additive, optional mask, IK pass, slot name, timing), each with a
  state machine: states, nested sub-state machines, Entry, Exit, Any State, default state. A state
  has a motion (clip or blend tree), speed and optional speed-multiplier parameter, cycle offset,
  optional motion-time parameter, mirror-free loop override (inherit/loop/once), tag.
- **R5 Transitions** (D5 semantics): ordered conditions (Float Greater/Less, Int
  Greater/Less/Equals/NotEqual, Bool If/IfNot, Trigger If), exit time (`hasExitTime`, fraction,
  every loop below 1), fixed or normalised duration, offset, interruption source (None, Current,
  Next, CurrentThenNext, NextThenCurrent), ordered interruption, can-transition-to-self (Any
  State). Triggers stay set until consumed or reset.
- **R6 Blend trees.** 1D, 2D Simple Directional, 2D Freeform Directional, 2D Freeform Cartesian,
  Direct; nestable; ≤ 32 children per tree; time scale per child; weights ≥ 0 summing to 1
  (Direct: the parameters, clamped ≥ 0); children phase-synced by normalised time; tree length =
  weight-averaged child length.
- **R7 Masks and additive.** `.mask`: humanoid body parts (Root, Body, Head, Left/Right Arm, Left/
  Right Hand, Left/Right Leg) and per-path weights 0..1. A bone takes its path weight when listed,
  else its body part's (non-slot bones inherit the nearest slot ancestor's part, other bones
  Body), else 1. Additive layers apply each clip as a delta against the clip's first frame
  (`additive_reference: first_frame | rest | <clip time>` per state). An additive layer at weight
  0, or a masked-out bone, is bit-identical to the base.
- **R8 Montages.** `.montage`: clip, slot, sections (name, start, next), blend in/out (seconds +
  ease), rate, own events. `playMontage` on a slot overrides the output of the layer with that
  slot (using its mask); a second montage on the slot blends the first out; sections chain;
  jump-to-section and set-next-section; notifies `ended(interrupted)` and `blendingOut`.
- **R9 Events.** Upstream crossing rules. Per layer `LeaderOnly` (default: the highest-weight clip
  of the layer's current state/transition fires) or `All` (every clip with weight > 0); montage
  events always fire; each fires once per Animator however many meshes it drives. Delivery: C++
  `onEvent(const AnimationEvent&)`, Lua `onEvent(event)`, and the method named like the event on
  the Animator entity's scripts (upstream's SendMessage rule). `play(state, time)` seeks fire none.
- **R10 Root motion.** Bone: `.anim` `root_motion.bone` > Animator `rootMotionBone` > humanoid
  hips > the rig's first root bone. Per clip the delta between the previous and the current clip
  time, split at wraps, blended by effective weight. Bake flags per clip (rotation, position Y,
  position XZ keep those parts in the pose). Delta is in the Animator's local space, applied to
  the Transform, or to S22 `setRootMotion` when a Character Controller is on the entity, or only
  reported to `onAnimatorMove` when a handler is set.
- **R11 IK stack.** After layers and montages, in model space, ordered: TwoBone (target entity or
  position, pole entity/position or keep bend), FABRIK (2–16 joints, iterations, tolerance),
  LookAt (≤ 4 bones, weights, clamp angle), FootIK (per-foot raycast, pelvis drop, sole align,
  spring smoothing), FootLock (locks while a clip float `contact` parameter or speed threshold
  holds, releases over a time). Each constraint's weight can be bound to a controller Float
  parameter; targets settable from Lua/MCP.
- **R12 Humanoid retargeting.** A clip whose source model (`model#clip`'s model; a `.anim`'s
  `source.path` or `preview.model`) is another humanoid model than the Animator's plays through
  D12 retargeting (slots + hips; with §12: fingers, eyes, jaw, folded slots, humanoid clips,
  state Mirror / Foot IK). `retarget: Off` disables. Avatar = upstream humanoid detection
  + `.humanoid` sidecar of the Animator's model. The Animation Player retargets through the same
  path (§12 "Retargeting").
- **R13 Modes.** Update: Normal, AnimatePhysics (evaluated in FixedUpdate), UnscaledTime. Culling:
  AlwaysAnimate, CullUpdateTransforms (time, states, events and root motion advance; sampling, IK,
  skinning skipped when none of its meshes was visible last frame), CullCompletely.
- **R14 Writing.** Bone local TRS to bone entities (`writeBones: All` default, `Exposed`: only
  listed paths and bones with non-rig children), morph weights, lattice offsets; skin matrices of
  the `ModelSkinBinding`s whose joints are all rig bones are written by the Animator
  (`animatorDriven`), never depending on bone entities.
- **R15 Overrides.** `clipOverrides` (clip → clip) reuses a controller with other clips.

### Platform & backends
- **R16** GL 3.3, GLES3/WebGL2, Metal, D3D11, Vulkan; Windows, Linux, macOS, Android, iOS, HTML5.
  No compute/SSBO. GPU skinning path and `MAX_BONES` unchanged.
- **R17** SIMD through `SimdFloat4.h`; Scalar compiled and tested; SIMD within 1e-5 of Scalar.
  `animation/` sources build with `-ffp-contract=off`.
- **R18** S06 with 0 workers (web without pthreads) runs serially, bit-identical.

### Performance (bench server E: 4 dedicated vCPU, ~10 % slower than the old E; Release)
- **R19 `AnimBench.Crowd200`**: 200 × 65-bone humanoid, all visible, `writeBones: Exposed` (4
  exposed); per character a Freeform Cartesian tree of 5 clips + a crossfade on 25 %, an upper-
  body override layer with mask, an additive layer, FootIK × 2 (raycasts against a ground
  collider), LookAt over 3 bones, root motion, events.
  - AnimatorSystem wall time ≤ 2.5 ms p95 with 3 workers + main (the original 1.5 ms at 8
    workers, scaled to 4 threads on E); main-thread share ≤ 0.4 ms p95; serial (0 workers)
    ≤ 8.8 ms p95; 0 heap allocations per frame after warm-up.
- **R20 Micro** (median of N): two-pose blend at 128 bones ≤ 1.1 µs and SIMD ≥ 2× Scalar; one
  65-bone clip sample ≤ 2.2 µs; state-machine update (20 states, 60 transitions) ≤ 1.1 µs;
  instance runtime state ≤ 8 KB.
- **R21** The RenderSystem per-joint skin loop does not run for Animator-driven bindings.

### Editor UX
- **R22 Animator window** (Window > Animation > Animator): Layers and Parameters tabs (add,
  rename, reorder, delete; layer weight, blending, mask, IK pass, slot; parameter defaults); a
  graph canvas (pan, zoom, grid snap, box select) with breadcrumb for sub-state machines; Entry
  green, Any State cyan, Exit red, default state orange; transitions as arrows (pairs offset,
  multiplicity mark). Right-click: Create State (Empty / From Clip / From New Blend Tree), Create
  Sub-State Machine, Make Transition, Set as Layer Default State, Delete. Drag `.anim` / model
  clips from the Project window onto the canvas to create states. Selection shows state,
  transition (conditions list) or blend tree in the Inspector. Every edit is one named project
  undo step.
- **R23 Live debugging.** In Play the window follows the selected Animator (or a pinned one):
  active state progress bar, transition progress, live parameter values (editable, triggers as
  buttons), layer weights, playing montage/section. In Edit mode, **Preview** evaluates the
  controller on the selected Animator (parameters editable), restoring the scene when it stops.
- **R24 Blend tree editor** (Inspector): child list (motion, threshold or position, time scale),
  Compute Thresholds / Compute Positions (from root-motion speed and direction), the 1D weight
  graph and the 2D sample diagram with a draggable parameter point and weight rings.
- **R25 Mask editor** (`.mask` Inspector): clickable body figure, transform tree from a chosen
  model with checkboxes and weights. **Montage editor** (`.montage` Inspector): clip, slot, rate,
  blend in/out, a section bar with draggable starts, next-section choice, events.
- **R26 Avatar**: the Animator drawer shows `Humanoid 22/22` (or Generic and the missing slots)
  with **Configure Avatar...** (upstream's bone-mapping dialog, writes `.humanoid`).
- **R27 Clip editing**: upstream's curves view gains tangent modes (Clamped Auto, Auto, Free,
  Linear, Constant, Flat, Broken) and draggable tangent handles; the clip dropdown lists the
  Animator's clips when the bound root has an Animator.
- **R28 Particle-aware preview**: Animator and clip edit-mode previews seek the Particle Systems
  under the previewed root to the preview time (S20 seek), on by default, setting
  `animation.previewParticles` (Preferences > Animation).

### Agent UX
- **R29 Lua**: `Animator` (D16) and `GameObject:getAnimator()`, documented in `lua-api.md` and the
  S23 API JSON, Unity names in `docs/unity-glossary.md`.
- **R30 MCP** (group `animation`, D17), each edit one undo step, validated before writing; the
  agent guide gets one line, details in `docs/mcp/guide/animator.md`; listing budgets hold.
- **R31 CLI**: every tool through S31 `jasperengine-editor call`.

### Data & compatibility
- **R32** `.controller`, `.mask`, `.montage`: JSON, `"format": "jasperengine.<kind>"`,
  `"version": 1`, top-level `guid` (S04), canonical text, unknown keys preserved, written with
  AtomicFile through the project command history, version migration table (v1 only today; a
  newer version refuses to load).
- **R33** Old scenes load and play bit-identically (no Animator → AnimatorSystem does nothing).
  The new component is additive (no scene version bump, S03). References are `AssetRef`s
  repaired on move/rename (S04).
- **R34** Exported games carry the assets as files; Factory emits `AnimatorComponent`.
- **R35** Hot reload (S05): editing a controller, mask, montage or clip rebinds live Animators,
  keeping the current state and time when the state's name still exists.

## 6. Design decisions

- **D1 Layering.** See §0. `AnimatorSystem` is registered after `ActionSystem` and before
  `PoseAnimatorSystem` (so a Pose Animator's weight blends over an Animator, as over a player).
- **D2 Rig and pose.** `AnimatorRig`: `bones[]` (entity, parent index, path, rest TRS), channel
  table (morph target / lattice point × axis → float slot). `Pose`: SoA float arrays `tx ty tz qx
  qy qz qw sx sy sz` padded to a multiple of 4, 16-byte aligned, plus `channels[]`. All pose
  buffers of an instance are allocated once at bind in one block (`PoseArena`); evaluation
  allocates nothing.
- **D3 Blend maths.** Override: per bone `lerp(base, layer, w·m)` (rotation nlerp with hemisphere
  alignment). N-way: weighted sum after aligning to the first contributor, then normalise.
  Additive: `q = base · nlerp(I, Δq, w·m)`, `t = base + Δt·w·m`, `s = base · lerp(1, Δs, w·m)`
  with `Δ` = clip ⊖ reference. Model space keeps non-uniform scale (`parent · T · R · S`).
- **D4 Sampling.** `RigClipSampler` maps each sampled track to a bone channel or float slot once
  per (rig, clip generation); per-instance `KeyCursor`s; values written straight into the pose
  arrays (no `TrackSample` vectors).
- **D5 Transition semantics (Mecanim).** Any State transitions first, then the current state's,
  in list order; at most one transition starts per layer per update; conditions ANDed; exit time
  `< 1` tested every loop at that fraction, `≥ 1` once; exit-time-only transitions fire on exit
  time; interruption sources as R5, `orderedInterruption` stops at the active transition's index;
  an interrupted crossfade freezes the in-flight blend as one snapshot pose; sub-state machines
  enter through their Entry transitions then default state; Exit returns to the parent machine
  and takes its transitions out of the sub-machine node (else the parent's Entry); `setFloat(name,
  v, damp, dt)` is critically damped.
- **D6 Blend-tree weights.** 1D piecewise linear; Simple Directional: the two angular neighbours
  plus the centre sample (barycentric); Freeform Cartesian and Directional: gradient-band
  interpolation (Johansen 2009; Directional in polar space with the angle term weighted by 2),
  pair data precomputed at compile time, O(n²) n ≤ 32, allocation-free; weights < 1e-3 dropped and
  renormalised.
- **D7 Execution (per frame).**
  1. **Prepare** (main): load/bind (pool polls), culling from last frame's visibility.
  2. **Wave** (`jobs::parallelFor`, chunks of 4 Animators): advance parameters/state machines and
     montages; sample, blend, montage override, IK (FootIK raycasts through
     `PhysicsLayers::raycast`, a read-only Jolt query; physics never steps during Update),
     root-motion extraction; model space; skin matrices `inv(meshRootSpace) · jointRootSpace ·
     IBM`; bone local TRS (`needUpdate`), morph and lattice writes. Each Animator writes only its
     own rig's entities; a second Animator claiming a bound entity gets a bind error.
  3. **Finalize** (main): root motion to the Transform or S22; events, state enter/exit and
     montage notifications in entity order, C++ subscribers then Lua.
  `ModelSkinBinding::animatorDriven` makes RenderSystem skip that binding. Lua never runs on
  workers. Profiler zones `Anim.Prepare`, `Anim.Wave`, `Anim.Finalize`.
- **D8 Defaults.** Normal update, CullUpdateTransforms, `writeBones: All`, `LeaderOnly`,
  `applyRootMotion: false`, `retarget: Auto`.
- **D9 Root motion → S22.** The Animator computes `deltaPosition` (world), `deltaRotation`; with a
  Character Controller it calls `setRootMotion(entity, Δp/dt, yaw/dt)`; otherwise the Transform
  gets the delta in Finalize. A per-frame delta above 50 m is dropped with a warning.
- **D10 Events** reuse `AnimationEvent` (`state` = the controller state's full path or the
  montage name). `AnimatorComponent` subscriptions: `onEvent`, `onStateEnter(layer, state)`,
  `onStateExit(layer, state)`, `onMontageEnded(montage, interrupted)`, `onAnimatorMove(dPos,
  dRot)` (when set, root motion is not applied automatically).
- **D11 IK.** Solvers run on the model-space cache and write back local rotations parents-first.
  Two-bone: law of cosines, pole projected onto the chain plane, reach clamped to 0.999 of the
  chain. FABRIK: shortest-arc rotation per joint, twist kept. LookAt: per-bone weighted
  shortest-arc toward the target, clamped. FootIK: ray from above the foot down, the pelvis drops
  by the larger foot offset, ankle aligned to the hit normal (clamped 30°), critically damped
  spring 0.08 s half-life. FootLock: world anchor while locked, blended out over `releaseTime`.
- **D12 Retarget.** `PoseRig` writes a slot's local rotation as `left · E · neutral` (E = joint
  angles, rig-independent). So for a source rig s and target t: `local_t = (left_t · left_s⁻¹) ·
  local_s · (neutral_s⁻¹ · neutral_t)`: two quaternion multiplies per slot, pre/post computed at
  bind (`PoseRig::slotFrame`). Hips position: the model-space offset from the neutral hips in
  canonical axes, scaled by `humanScale_t / humanScale_s`. Non-slot bones keep the target rest.
  Source rigs come from the source model's glTF nodes (`modelNodePaths`) and its `.humanoid`.
- **D13 Assets.** `ControllerIO`, `AvatarMaskIO`, `MontageIO` (engine): parse (lenient, warnings
  with JSON paths like `.anim`), canonical write, normalize, migrate. `AnimatorAssetPool`
  (engine): shared immutable compiled assets keyed like `AnimationClipPool`, with generations.
  Editor: `AnimatorAssetDocument` (one per path, commit through `WriteFilesCmd`, a project undo
  step, as `AnimationClipDocument`).
- **D14 Graph canvas.** `editor/window/widget/GraphCanvas.{h,cpp}`: our own (no node-editor
  library: imnodes lacks zoom and node-to-node arrows; imgui-node-editor pins ImGui internals).
- **D15 Tangents.** Upstream keys carry Hermite tangents. Tangent modes are editor operations
  that compute and store tangents (Clamped Auto = Unity's monotone clamp, Auto = Catmull-Rom,
  Linear, Constant = step, Flat = 0, Free/Broken = handle drags). Rotation tangents are computed
  per quaternion component; their handles are read-only in the Euler view. The mode per key is
  kept in the editor's clip metadata (`editor.tangent_modes`, an unknown-key-preserved block).
- **D16 Lua.**
  ```lua
  local a = GameObject(self.scene, self.entity):getAnimator()   -- or Animator(scene, entity)
  a:setFloat("Speed", v [, damp, dt]); a:setInteger(n, i); a:setBool(n, b)
  a:setTrigger(n); a:resetTrigger(n); a:getFloat(n); a:getInteger(n); a:getBool(n)
  a:play("Base Layer.Run" [, layer, normalizedTime]); a:crossFade(state, normalizedDuration [, layer, offset])
  a:crossFadeInFixedTime(state, seconds [, layer, offset])
  a:getCurrentState(layer) -- {name, fullPath, tag, normalizedTime, length, loop}
  a:isInTransition(layer); a:setLayerWeight(i, w); a:getLayerWeight(i); a:getLayerIndex(name)
  a:playMontage(path [, rate, section]); a:stopMontage([blendOut]); a:isMontagePlaying()
  a:montageJumpToSection(n); a:montageSetNextSection(from, to)
  a:setIKTarget(name, pos [, weight]); a:setIKWeight(name, w); a:setLookAt(pos [, weight])
  a:getBoneWorldTransform(nameOrSlot) -- position, rotation
  a.speed; a.applyRootMotion; a:getRootMotion() -- deltaPosition, deltaRotation
  a.onEvent, a.onStateEnter, a.onStateExit, a.onMontageEnded, a.onAnimatorMove
  ```
  Unknown parameters or states log once per name per Animator and do nothing.
- **D17 MCP tools** (group `animation`):
  - `get_animator {entity | controller}`: the controller (parameters, layers, states,
    transitions, trees) with diagnostics (unreachable states, unknown parameters, missing clips);
    with an entity also the Animator's settings, rig/avatar status, and in Play (or preview) the
    live layers, states, transitions, parameters, montage and IK.
  - `edit_animator_controller {path, create?, ops[]}`: `add/remove/rename_parameter`,
    `add/set/remove_layer`, `add/set/remove_state`, `add/remove_sub_state_machine`,
    `add/set/remove_transition` (from state, `Any State` or `Entry`; to state or `Exit`),
    `set_default_state`, `set_blend_tree`, `from_player {entity}`. One project undo step.
  - `control_animator {entity, parameters?, triggers?, play?, crossfade?, montage?, stop_montage?,
    ik?, preview?}`: Play mode, or the edit-mode preview when `preview` is set.
  - `edit_avatar_mask {path, create?, body_parts?, paths?, from_model?}`.
  - `edit_montage {path, create?, clip?, slot?, rate?, blend_in?, blend_out?, sections?, events?}`.
  - `sample_animation_pose {entity, state?, parameters?, time?, bones?}`: bone world TRS of an
    evaluated pose (edit or play), for agents to verify.
  The Animator component itself is edited with `add_component` / `set_properties` (S08).
- **D18 File formats** (canonical JSON, 2-space indent, fixed key order):
  ```json
  {"format": "jasperengine.controller", "version": 1, "guid": "…",
   "parameters": [{"name": "Speed", "type": "float", "default": 0}, {"name": "Jump", "type": "trigger"}],
   "layers": [{"name": "Base Layer", "weight": 1, "blending": "override", "mask": null, "slot": "",
     "ik_pass": true, "state_machine": {"default_state": "Locomotion",
       "entry": [], "any_state": [{"to": "Hit", "conditions": [{"parameter": "Hit", "mode": "if"}], "duration": 0.1}],
       "states": [{"name": "Locomotion", "position": [240, 120], "speed": 1, "tag": "",
         "motion": {"blend_tree": {"type": "freeform_cartesian", "parameter_x": "VelX", "parameter_y": "VelZ",
           "children": [{"clip": {"path": "models/hero.gltf#Run", "guid": ""}, "position": [0, 1]}]}},
         "transitions": [{"to": "Jump", "conditions": [{"parameter": "Jump", "mode": "if"}],
           "has_exit_time": false, "exit_time": 0.9, "fixed_duration": true, "duration": 0.15,
           "offset": 0, "interruption_source": "none", "ordered_interruption": true}]}],
       "sub_state_machines": []}}]}
  ```
  `.mask`: `{"format": "jasperengine.mask", "version": 1, "guid", "body_parts": {"head": true, …},
  "paths": [{"path": "Hero/Hips/Spine", "weight": 1}]}`. `.montage`: `{"format":
  "jasperengine.montage", "version": 1, "guid", "clip": {"path", "guid"}, "slot": "UpperBody",
  "rate": 1, "blend_in": {"time": 0.15, "ease": "linear"}, "blend_out": {…}, "sections": [{"name":
  "A", "start": 0, "next": "B"}], "events": [{"time", "function", "args"}]}`. `.anim` (optional):
  `"root_motion": {"bone": "Hero/Hips", "bake_rotation": false, "bake_position_y": true,
  "bake_position_xz": false}`.

## 7. Acceptance criteria

Unit tests in `engine/tests/core/animation/` (`Animator*.cpp`), editor tests in
`editor/tests/animation/`, MCP tests `tools/mcp/tests/test_animator*.py`, fixture
`tools/fixtures/animator/` (upstream's hero and robot humanoids plus generated clips).

- **AC1 (R1, R2, R14)** `AnimatorRig.*`: binding by path, parents-first order, rest from scene,
  rebinding on rename keeps state; `RigClipSampler.MatchesPlayer`: one clip through the Animator
  (one state, weight 1) writes the same local TRS (≤ 1e-6) as the Animation Player.
- **AC2 (R6, D6)** `AnimatorBlendTree.*`: weights ≥ 0 and sum 1 ± 1e-6 on a 64×64 grid for each
  2D type; a parameter on a sample gives it weight 1; 1D exact at thresholds; a 20-sample tree
  uses every sample; normalised-time drift ≤ 1e-4 after 10 s.
- **AC3 (R4, R5, D5)** `AnimatorStateMachine.*`: ExitTimeEachLoop, ExitTimeOnce, AnyStateFirst,
  TriggerConsumedOnce, TriggerPersistsUntilUsed, InterruptionNextThenCurrent,
  OrderedInterruption, SubStateEntryExit, NoTransitionToSelfUnlessAllowed,
  InterruptedBlendIsContinuous.
- **AC4 (R3, R7)** `AnimatorPoseOps.SimdMatchesScalar` (1e-5, 10k random poses),
  `AnimatorLayers.MaskOverride`, `AnimatorLayers.AdditiveZeroWeightBitIdentical`,
  `AnimatorLayers.AdditiveReferenceFrameNoChange`.
- **AC5 (R11, D11)** `AnimatorIK.*`: TwoBone within 1 mm and mid joint on the pole side;
  unreachable → straight chain toward the target; FABRIK converges ≤ 1 mm on 8 random cases;
  LookAt never exceeds its clamp; non-uniform scale chains; `FootIK.SlopeGrounding` (20° ramp:
  soles within 1 cm after 30 frames, pelvis never above the animation's); `FootLock.NoSlide`
  (≤ 2 mm drift while locked).
- **AC6 (R10, D9)** `AnimatorRootMotion.*`: LoopWrapContinuous (10 laps = 10 × displacement
  within 0.1 %, no backward step), ConfigurableBone, BakePositionY; MCP: a controller-driven
  character with a Character Controller follows root motion up a ramp.
- **AC7 (R12, D12)** `AnimatorRetarget.SameRigIdentity` (≤ 1e-5), `CrossAxisConvention` (hero →
  robot walk: hand and foot world directions within 5° of the source's, hips scaled).
- **AC8 (R8, R9)** `AnimatorMontage.*` (sections chain, jump, set-next, blend out on a second
  montage, ended(interrupted)), `AnimatorEvents.*` (leader-only, all, montage, seek fires none,
  `EventsOncePerAnimator` with 2 models and 3 skinned meshes over 300 frames).
- **AC9 (R32, D13)** `ControllerIO.RoundTrip`, `AvatarMaskIO.RoundTrip`, `MontageIO.RoundTrip`
  (write → parse → write identical), unknown keys preserved, newer version refused, lenient
  warnings with JSON paths; `AnimatorAssetIO.FuzzNoCrash` (mutated texts under ASan).
- **AC10 (R33)** `AnimatorCompat.PlayerScenesUnchanged`: the upstream player/entity-animation
  fixtures give byte-identical Transforms with this branch; existing upstream animation and pose
  suites pass unchanged.
- **AC11 (R13, R14, R21)** `AnimatorCulling.CullUpdateTransforms`, `AnimatorWrite.Exposed`,
  `AnimatorSkin.MatchesRenderSystem` (Animator-written `bonesMatrix` equals RenderSystem's from
  the same bone transforms ≤ 1e-5); goldens `animator_poses` (hero at three controller poses,
  glcore and Vulkan).
- **AC12 (R19–R20)** `je-bench-anim` (`AnimBench.Crowd200`, `AnimBench.Micro*`) on E meets the
  budgets (median of N runs recorded in `docs/perf/animator.md`); `AnimatorSystem.ZeroAllocSteady
  State` (600 frames), `AnimatorSystem.ParallelEqualsSerial` (bit-identical over 600 frames).
- **AC13 (R22–R28)** editor tests: each Animator-window operation is one named project undo step
  that undoes exactly; tangent modes reproduce reference values (Clamped Auto table);
  `test_animator_editor.py` drives the window (create states by drag, transitions, preview) and
  screenshots it.
- **AC14 (R29–R31)** `test_animator.py` (tools and Lua only): build a controller (idle,
  locomotion tree, jump, any-state hit), assign it, Play, set parameters, check transitions via
  `get_animator`, Lua `onEvent` counts, montage `A→B`, `sample_animation_pose`; one call through
  the CLI. `test_protocol.py`, budget, conventions and docs tests pass; smoke stays green; the web
  gate and the Android emulator run the animator test project.

## 8. Dependencies & conflicts

- Uses: S03 (AtomicFile, versions), S04 (GUIDs, AssetRef), S06 (parallelFor), S07 (scheduler),
  S08 (reflection tables), S20 (SimdFloat4, seek), S22 (`setRootMotion`), S24 (named project
  undo), S25 (CommandCatalog, SettingsRegistry), S26 (picking in the mask/avatar editors), S31
  (CLI, conventions).
- Hotspots: `editor/Catalog.h` (append `AnimatorComponent`), `EntityRegistry`, `Scene.cpp`,
  `RenderSystem.cpp` skin loop, `ModelComponent.h` (`ModelSkinBinding`), `AnimationWindow*.cpp`,
  `InspectorWindow.cpp`, `GameObjectClassesLua.cpp`, `docs/mcp/TOOLS.md` (regenerated),
  `budget.json`, Xcode project.

## 9. Risks & defaults

- **Two players on one hierarchy.** Default: the Animator wins on the bones it binds; Inspector
  warning.
- **Retarget without muscle space**: slot-only transfer (22 slots), fingers keep rest.
- **FootIK raycasts in the wave**: Jolt queries are read-only during Update; cap 2 per character.
- **Web without threads**: serial budget; documented.
- **Graph canvas effort**: accepted (D14).

## 10. Deviations

1. **D8 culling default**: `AlwaysAnimate` (Unity's default), not `CullUpdateTransforms`: an
   Animator seen only by an off-screen camera, a render target or the editor's Scene view must not
   freeze by default. `CullUpdateTransforms` and `CullCompletely` are there to choose.
2. **No per-layer "IK pass"** (R6, R22, the `ik_pass` key): the IK stack runs once, after every
   layer and montage (R11); a per-layer flag would have nothing to switch. The key is not part of
   the format (read, it is an unknown key: a warning).
3. **Lua `getBoneWorldTransform`** is `getBonePosition` / `getBoneRotation` (+ `hasBone`): two
   values from one call are awkward through LuaBridge; Unity's `GetBoneTransform` returns an object.
4. **`clipOverrides[].replacement`** is a plain path (no GUID): the override list is scene data
   edited as text; a moved replacement is reported unbound (warning) like a missing clip.
5. **R28 preference**: `scene_view.preview_particles` (Preferences > Scene View > Animation), not a
   new `animation.previewParticles` page: one toggle did not justify a Preferences page.
6. **R27 tangents**: one mode per key (all its components), stored as the key's `"tangent_mode"`
   in `.anim` (written only with tangents; Constant keeps the step ease with tangents, the one
   ease a Hermite track keeps). Rotation tangents are quaternion tangents: their handles show
   read-only, the modes apply. Unity's per-curve modes and Euler handles are not reproduced.
7. **R12 targets**: the target humanoid is built from the Animator's model file (its nodes and
   `.humanoid` sidecar, the same source a clip of that file uses) and mapped onto the rig's bones
   by path: the rig holds a subset of the nodes, which changed what the auto-detect found.
8. **R20 runtime state** is `AnimatorInstance::runtimeStateBytes()`: the instance, parameters,
   layer / montage / IK runs and output buffers; the rig, poses, per-bone masks and clip bindings
   grow with bones and clips and are counted by the Crowd200 memory, not by the 8 KB.
9. **Test names**: AC1-AC11 are covered under the suites `AnimatorAssetIO`, `AnimatorPoseOps`,
   `AnimatorBlendTree`, `AnimatorStateMachine`, `AnimatorIK`, `AnimatorFootIK`,
   `AnimatorRetargeting`, `AnimatorSystemTest` (root motion, montages, events, culling, writing,
   skins, player compatibility), `AnimatorSystemAllocations`, `AnimatorSystemDeterminism`,
   `ClipTangentModes`, `AnimatorGpu`; the editor's `AnimatorEditor`.
10. **`edit_animator_controller` ops** are free-form objects in the tool's schema (the per-op
    fields would not fit the 1,500-character tool budget); the tool checks each op's fields
    itself (an unknown field or a wrong kind is refused by name) and the guide page lists them.
11. **Component order**: `AnimatorComponent` registers after the navigation components (67-72)
    and S18's splines (73-76): component 77; its Catalog reload flag is bit 45 (S18 took 44). No
    scene or prefab format bump.

## 11. Progress

- [x] Re-scope and revised spec.
- [x] Runtime: asset formats and IO (`AnimatorAssetIO`), pool, SoA poses with SIMD back ends,
  rig binding, compiled controllers, blend trees, state machines, layers / masks / additive,
  montages, events, IK stack (TwoBone, FABRIK, LookAt, FootIK, FootLock), root motion,
  humanoid retargeting, `AnimatorSystem` (parallel wave, skins, culling, Exposed writes).
- [x] Component, S08 reflection, Lua (`Animator`, `AnimatorStateInfo`), script events.
- [x] Editor: Animator window (graph canvas, layers, parameters, preview, live debugging),
  Inspector (component, state / transition / layer / blend tree, controller / mask / montage
  assets), Project window assets and icons, menus and commands, curves-view tangent modes and
  handles, Animator clips in the clip dropdown, particle-aware previews, asset renames.
- [x] MCP tools (group `animation`), the guide line, `docs/mcp/guide/animator.md`, user docs,
  Lua API and glossary.
- [x] Tests: core (`Animator*`, `ClipTangentModes`), GPU golden (`AnimatorGpu`), editor
  (`AnimatorEditor`), MCP (`test_animator.py`); bench `je-bench-anim`.
- [x] Gate on oe-port `40bec8043` (S18 splines, BF8) merged: linux WERROR editor build; core
  suite 2,329 passed; editor suite 1,375 (B); Vulkan (lavapipe) `Animator*` / `ClipTangentModes*`
  incl. the GPU golden, 68 passed; ASan+UBSan animation suites, 146 passed; web gates (simd128
  and scalar) pass with the same pose; Android emulator (`tools/test-projects/animator`) passes;
  smoke passes; E bench: all budgets hold, Crowd200 p95 2.37 ms median of five
  (`docs/perf/animator.md`; about 0.1 ms under its 2.5 ms budget, so measure it on an idle E: a
  batch beside another job on E read 3.0 and 3.7 ms); Xcode project synced. Full MCP suite (`MCP_SUITE=all`): every S21
  test and the guide, budget, docs, reflection and ECS tests pass; the failures left are not
  S21's: they fail on `sync/origin-main-3` too (`test_pose_runtime`, `test_lattice` screenshot,
  `test_post_fx_golden`, `test_shadows`, the CLI `check` `map::at` abort, `test_place_physics_cli`,
  `test_shader_library`, `test_synthesize_texture`), are oe-port's own (`test_scene_inputs`
  expects an older shader message; the fluid / UI-effect "no notice" exports get S18's
  unconditional `licenses/OpenEngine-LICENSE.txt`), or are flaky (`test_animation_assets`
  autosync and `test_post_processing_review_fixes` LUT: pass and fail across three reruns).

## 12. Upstream absorption (sync 4)

Upstream (origin/main, "anim layers", `docs/superpowers/specs/2026-10-31-anim-layers-design.md`)
built Animation Player layers with Avatar Masks, and Animator Controllers played by the Animation
Player itself, with their own `.controller` / `.mask` readers, Animator window and MCP tool
(`edit_animator`). S21 is the main implementation; upstream's unique parts were absorbed onto it.

**What was kept from upstream, on S21's runtime.**
- Animation Player layers (`AnimationPlayerComponent::layers`, `clips[].layer`; weight, mask,
  override / additive, per-layer `play` / `crossFade` / `queue`, `setLayerWeight(name, w, fade)`,
  `PlayMode.STOP_ALL`, events only from weighted layers) with upstream's semantics, composed with
  S21's per-element maths (`poseBlendRotation` / `poseAdditive*`, the scalar lanes of the SIMD
  kernels) and S21's masks (`AvatarMaskWeights.h`). The Inspector's Layers table, clip Layer combo,
  Create Mask from Selection, mask coverage in Bindings, the Animation window's Preview over and
  masked rows, MCP `set_properties` of layers, `get_animation {entity}` layers and coverage,
  `get_animation {mask, root}` and `{controller}`.
- Upstream's Inspector parts: the Avatar Mask figure (fractional parts, Default, Root
  Hierarchy, Preview Model, Users), the controller asset's validation (`validateController`: clips
  that do not load, mistyped parameters, dead ends, unreachable states, transitions never taken,
  unused parameters, non-Lua names), "Create Animator Controller from Clips" (an Animator added,
  the player disabled, one scene step). The Animator window gained upstream's behaviours: renames
  that follow their references, Copy / Paste, typed paths checked, a Problems strip whose click
  selects the item, the current layer followed by name across layer list changes (a stale
  selection dropped), parameter removal taking its conditions (MCP too), state removal taking the
  transitions into it (MCP too), out-of-range numbers refused by `edit_animator_controller`, whose
  result now carries `warnings` and `users`.
- Upstream's controller-on-player API, as a **handover**: a disabled Animation Player on a
  GameObject with an Animator forwards `play` / `crossFade` / `stop` / `isPlaying` / `setEnabled`,
  the parameters, the state reads (`ControllerStateInfo`), `playInLayer` / `crossFadeInLayer`,
  layer weights and its `onStateEnter` / `onStateExit` / `onEvent` to the Animator
  (`object/AnimationPlayer.h`). `GameObject:getAnimator()` returns the Animator (the player without
  one); the Animator's Lua class also answers upstream's names (`getCurrentStateName`,
  `getCurrentStateInfo`, `getNextStateInfo`, `isInTransition([layer])`, `playInLayer`,
  `crossFadeInLayer`, `hasController`, `hasParameter`, layer weights by name).

**Formats.** `.controller` and `.mask` are version 2 (`CONTROLLER_VERSION`, `MASK_VERSION`); both
dialects wrote `"version": 1` with the same header, so a version 1 file is told apart by its
members (`AnimatorAssetIO.cpp`, `AnimatorFileDialect`): a controller is upstream's when a layer has
upstream's members (`states`, `transitions`, `blend`, `default_state`, `any_state` as an object,
`mask_guid`, a string `mask`) and none has S21's (`state_machine`, `blending`, `slot`); a mask is
upstream's when it has upstream's members (`default`, `humanoid`, `transforms`, `preview`) and no
S21 member, or upstream's are informative and S21's are not. Upstream's files are read by
`UpstreamAnimFormats` and converted losslessly: every member lands in an S21 member or in the
unknown keys of the matching S21 object (unknown keys are kept at every level now, and written
back; an unknown key colliding with an S21 member is kept as `upstream_<key>`). Controller: flat
states and transitions nest into each layer's state machine (`from: "any"` -> Any State),
`blend` -> `blending`, string references + `*_guid` -> `{path, guid}`, `interruption` ->
`interruption_source`, upstream's defaults made explicit (`has_exit_time` true,
`can_transition_to_self` true on Any State), `wrap` -> `loop` (new `ping_pong`, `clamp_forever`,
`repeat`), additive reference -> `clip` (new: the .anim's own `additive_reference`), a child's
`speed` -> `time_scale`, `mirror` / `foot_ik` / `retarget` / `automate_thresholds` kept as unknown
keys, positions kept (a grid for states without one). Mask: `default` -> default, `humanoid` ->
`body_parts` (upstream's arm also weighs the hand; fingers are parts of their own now), `transforms`
-> `paths` (with their `children`, upstream's default true), `preview.model`, `root_hierarchy`
true. S21's version 1 masks convert too (default = the body's weight, every part set, a hand's
part also below it, paths exact). Readers report `version`, `dialect` and `converted()`; the
editor, MCP (`read_as`) and `check --anims` note a converted file, and the next save writes version
2. A save never drops data: the conversion tests read, save and re-read every upstream member.

**Mask rule (version 2).** A GameObject's weight: its `paths` entry, else its nearest ancestor's
entry with `children`; else (a humanoid rig) its body part (slot bones by slot: body, head,
shoulder to forearm `<side>_arm`, the hand bone `<side>_hand`, legs; a non-slot bone takes its
nearest slot ancestor's part, below a hand `<side>_fingers`; with `root_hierarchy` the root and
what is between it and the hips: `root`); a part not set weighs `default`; else `default`. Root
motion weighs the root part. Animator layers and Animation Player layers use the same function.

**Migrations.** Scene version 5 -> 6 and prefab 6 -> 7 (`StreamMigrations.cpp`
`moveAnimationPlayerControllers`): a player's `controller` / `controllerGuid` become an
`animator` component on the same entity playing that controller (enabled as the player was,
`retarget: off` as upstream played it), and the player is disabled so it hands over; prefab
instance overrides of a player's controller become an added Animator. Players with layers load
unchanged (the codec writes `layers` and `clips[].layer`, `StreamComponents.cpp`); a `controller`
key left in a current file is not read (a warning).

**Clip property tracks on the Animator (anim-properties, sync 4 with origin/main 27d562a6b).**
Upstream's property tracks (`ClipChannel::Property`: Catalog component properties, GameObject
active, enabled checkboxes, Lua script fields) play on the Animator as they play on the Animation
Player: the same bindings (`PropertyBindings.h`, bound on the entity the path names, `""` the
root) and key space. Each bound (entity, property) is `width` float channels of the rig
(`AnimatorRig::properties`), sampled by `sampleClip` (swizzle tracks key one component, a whole
track the components no swizzle track keys, RGB keys of a Color4 an opaque alpha, a discrete
binding of interpolating keys stepped) and blended like morph weights by states, transitions and
layers; layer masks pass them (masks select transforms). `AnimatorSystem::writePose` writes the
values that changed, a discrete property rounded to the nearest value (the heavier side of a
two-way blend), deferred rows (GameObject active, a script's enabled) after the other writes; the
rest is taken before the first write and put back by the preview's restore. An entity keyed only by
property tracks is not a bone. Test: `AnimatorSystemTest.PropertyTracksPlayOnTheAnimator`.

**Deviations from upstream (S21's and Unity's rules win).**
1. A destination's `onStateEnter` fires when its transition starts, the source's `onStateExit`
   when it ends (upstream: both at the end).
2. An interrupted transition freezes the blended pose and fades it out under the new one.
3. A blend tree state's length is the weighted average of its children's; `normalizedTime`
   excludes the cycle offset.
4. A transition with neither conditions nor exit time is never taken (validated), not taken at
   once.
5. A controller with no layers has none (upstream added a Base Layer on read).
6. Parameter names that are not Lua identifiers are kept and warned about (scripts pass names as
   strings), not refused.
7. Morph-weight and lattice channels on entities that are not bones follow the layer's mask like
   bones.
8. The last layer of a controller cannot be removed; the base layer may move (Unity's rule).

**Tests.** Core: `AnimatorUpstreamFiles` (written red first: an upstream controller read as empty
layers and a save wiped it, an upstream mask let every bone through; then the conversion member
by member, the converted file playing like the upstream one, the humanoid mask conversion, S21's
version 1 files), `Layers` (upstream's layer tests), `Controllers` (upstream's controller tests
through the handover). Editor: `AnimationLayersEditor`, `AnimationLayersUi`, `AnimationLayersFigure`,
`AnimatorWindow` (upstream's window tests on S21's window), `StreamMigrations.SceneV5ToV6*`. MCP:
`test_animation_layers.py`, `test_animator_controller.py`, `test_animator_window.py`,
`test_checks.py` (`--anims`).

### Retargeting (anim-retarget)

Upstream's anim-retarget (`docs/superpowers/specs/2026-10-31-anim-retarget-design.md`) made the
Animation Player retarget through its own path: generic clips converted to humanoid clips in the
clip pool (`"<clip>@humanoid"`) and realised on the player's rig, with humanoid clips, Avatars,
Mirror and Foot IK. S21's retargeting wins, and there is **one retarget path** for the Animator
and the Animation Player: `animation/AnimatorRetarget.{h,cpp}`. A clip becomes a
`HumanoidSample` (a humanoid clip's tracks, or a generic clip read at play time through its
source model's `RetargetSource`: D12's source half, `E = A_s * local * B_s`), and
`realiseHumanoid` poses the target (D12's target half, `left_t * E * neutral_t`), then the
Animator writes its rig's pose (`writeRealised`) and the player its bone entities
(`HumanoidBinding` outputs). Nothing else turns clip values into another rig's bones: the pool's
`@humanoid` form is gone (a Retarget entry loads its clip as it is), and Convert to Humanoid Clip
writes keys with the very source maps the play-time path applies (a converted clip and its generic
original play the same, `HumanoidRetargetTest` checks it on every conversion).

**Absorbed from upstream, on that path.**
- Humanoid clips (`.anim` version 2: `humanoid` / `humanoid_position` tracks, `{"angles"}` keys,
  the `humanoid` header): the Animator plays them (`AnimatorRig`'s sampler leaves humanoid tracks
  to the path; their generic tracks bind by path), as does the player.
- The Avatar (`.humanoid` version 2: `animation_type`, `extra_bones`, `twist`): the Animator's
  avatar is its model's `RetargetSource` (the skin skeleton of `pose::buildSkeletonFromGltf` with
  its bridges, the sidecar's bones, extra bones and twist corrections), mapped onto the rig's bones
  by path (deviation 7), else one built from the rig's own bones. Fingers, eyes and jaw retarget;
  a slot the target lacks folds into its next present one (S21 dropped it); a slot the clip lacks
  is neutral; an Animation Type: Humanoid model serves humanoid clips (the pool's conversion).
- Mirror and Foot IK: S21 state fields `mirror`, `foot_ik` and `retarget` (written when true;
  upstream's state flags convert into them, no longer unknown keys) and a blend child's `mirror`
  (it flips its state's). A state with any of them plays its clips through the humanoid path
  whenever the Animator has an avatar (its own model's clips read through the avatar), so Mirror
  works on the model's own clips (deviation 9 below); `retarget` retargets whatever the Animator's
  Retarget (a migrated upstream controller has the Animator's Retarget Off, as upstream played
  it, and keeps its states' Retarget). A clip used by states that ask for the path and by states
  that do not gets one clip slot of each (`CompiledController::clipHumanoid`). The Animator
  window's state inspector has Retarget / Mirror / Foot IK, the blend tree table a Mirror column;
  "Create Animator Controller from Clips" copies each entry's flags onto its state; MCP
  `edit_animator_controller` takes `mirror`, `foot_ik`, `retarget` on states and `mirror` on
  children, `get_animator` lists them; Lua `AnimatorStateInfo.mirror` / `.footIK`.
- The Avatar Configuration window and the Model Inspector's Rig foldout (upstream's), now also
  what the Animator drawer's **Configure Avatar...** opens; Convert to Humanoid Clip (the Project
  window's `.anim` menu too) and Bake to Humanoid Clip; Lua `Model:getHumanoidInfo`; MCP
  `set_humanoid` Avatar fields, humanoid tracks in `edit_animation`, `get_animation`'s
  `retarget_report` and `realised` (computed through the play-time binding of the entry).

**Lost in the merge, restored.** The player entries' `retarget` / `mirror` / `footIK` were on
`AnimationPlayerClip` but not in the scene codec (`StreamComponents.cpp`: an upstream scene's
flagged entries loaded unflagged and a save dropped them), the S08 tables (`set_properties
clips[i].retarget` was refused) or the exported Factory code (exported games played them as
generic entries); `RuntimeFields.gen.inc` and `docs/mcp/TOOLS.md` are regenerated.

**Rules and deviations.**
9. Mirror and Foot IK apply whenever the Animator has a humanoid avatar (Unity: every clip of a
   humanoid avatar is humanoid); upstream applied them to humanoid clips and Retarget entries only
   (its player keeps that rule: a generic entry without Retarget ignores them).
10. A state's Foot IK solves per sample before blending (upstream's order: the same pose on both
    players), with the pose system's limb IK; it is not the IK stack's FootIK constraint (ground
    raycasts after every layer, R11).
11. A Retarget entry retargets at play time: no conversion wait (H9), the source rig built on the
    first bind (cached per model and sidecar text) and rebuilt after a sidecar edit; a missing
    source model fails the state ("retarget: the clip names no source model (set its Preview
    Model)") and binds nothing.
12. The Animator binds a retargeted generic clip's generic tracks (props, unmapped leaf bones)
    by path only; the player also tries a unique bone name (upstream's 7.6).

**Tests.** Core: `AnimatorHumanoid` (written red first, commit "Retarget absorption, red": the
Animator left the robot at rest on a humanoid clip, on an Animation Type: Humanoid model's clips
and on upstream's state Mirror / Foot IK / Retarget, ignored the Avatar's twist, dropped the
hero's upper chest instead of folding it and left the fingers at rest; then each matches the
Animation Player within 0.05 degrees and 0.1 mm), its lossless-file tests (upstream controllers
with the flags, the earlier absorption's unknown-key form, `.anim` v2 and `.humanoid` v2 round
trips), `HumanoidRetargetTest` and `HumanoidModelTest` (upstream's, un-parked: play time and
conversion agree, S21 masks for the masked humanoid layer, a Retarget entry without a source
model), `AnimatorRetargeting` (AC7 unchanged). Editor: `AnimatorEditor` (the player's flags on
the states), upstream's `HumanoidRetargetEditor`. MCP: `test_animation_retarget.py`.
