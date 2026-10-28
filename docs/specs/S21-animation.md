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
- Humanoid muscle space, mirroring, sync layers, Unity `.controller`/`.anim` import (README: no
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
  D12 retargeting (slots + hips). `retarget: Off` disables. Avatar = upstream humanoid detection
  + `.humanoid` sidecar of the Animator's model.
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

## 11. Progress

- [x] Re-scope and revised spec.
