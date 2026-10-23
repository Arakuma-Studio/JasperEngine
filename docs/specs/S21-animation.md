# S21 — Animation

Wave 2. Owner stream: animation. Status: spec.

## 1. Goal

Add a pose-based animation runtime next to today's entity-per-track system. It has a
Mecanim-style Animator Controller (parameters, layered state machines, blend trees), masks,
additive layers, montages, IK, root motion, animation events and humanoid retargeting. The
editor gets an Animator window, a clip dope sheet and curve editor, and avatar and mask editors.
Lua and MCP can drive all of it. Old scenes keep playing exactly as they do now, and an explicit
upgrade moves a model to the new runtime. The goal is gameplay characters ("upper body aims while
legs run, feet stay on the slope, an attack plays and fires a hit event once") at 200 characters
per frame, on every platform.

## 2. Scope

**In scope**
- `engine/core/animation/`, the new runtime:
  - `Skeleton`, an SoA `Pose`, and SIMD pose operations;
  - clip data and sampler, and the shared `Curve` (`engine/core/math/Curve.h`, shared with S20 D5
    and S27);
  - the compiled controller and its runtime instance, blend trees, layers and masks;
  - montages and slots, the IK constraint stack, root motion and events;
  - the humanoid avatar, auto-mapping and retargeting;
  - a binary codec for cooked animation assets.
- The `AnimatorComponent`, the `AnimatorSystem` and the `object/Animator` wrapper. A model upgrade
  path from the legacy track entities.
- Clip channels: bone TRS, morph weights, generic property curves (bound through S08
  reflection), discrete sprite-frame tracks and event tracks.
- 2D: an Animator over a *generic rig built from the entity hierarchy*, for cut-out characters
  made of Sprites. Sprite flipbooks can be driven from controller states.
- glTF `extras.clip` schema v1 (events, loop, speed, root motion, translation mode), honoured in
  full.
- Editor: the Animator window (graph canvas, layers and parameters, live debugging), the blend
  tree editor, and the avatar and mask editors. The Animation window gains clip editing with a dope
  sheet, a Curves tab (the shared curve widget), an event lane and a montage section lane.
- Lua API, MCP tools and resources, docs, tests and benchmarks.

**Out of scope**
- GPU or compute-shader animation sampling (OpenEngine's `GPUAnimationDataStore`, rejected in
  04-gameplay-systems.md:212). Skinning stays on today's GPU path (vertex uniforms or
  `SKINNING_TEXTURE`).
- Their graph runtime and `AnimationGraphSerializer` as a format. Their retained-UI editor panels
  (`AnimationWindowPanel.cpp`, `CurvesGraphView.cpp`, `AnimationGraphPanel.cpp`; README
  do-not-copy list: their editor UI toolkit). Their `SkeletonModifierStack` (8/9 kinds are empty
  stubs, `SkeletonModifierStack.cpp:64-75`).
- Humanoid muscle space, mirroring, sync layers with timing, Unity `.controller` and `.anim` import
  (README: no Unity importer), ragdolls, clip compression beyond keyframe reduction, and 2D
  sprite-mesh skinning (deformable sprites with bone weights). The last one goes to a later spec.
  The pose runtime does not block it.
- Rewriting tweens, `SpriteAnimationComponent`, particles (S20) or the legacy `Animation`
  timeline. They keep working unchanged.

## 3. Current state

- **Model.** Animation is action-based. `Animation` (`engine/core/action/Animation.h`) is an
  `Action` holding `ActionFrame`s (`component/AnimationComponent.h`: start, duration, action
  entity, editor track). Every glTF channel becomes its own entity with
  `KeyframeTracksComponent` (times plus per-segment `EaseType`) and one of
  `{Translate,Rotate,Scale,Morph}TracksComponent`. Hermite tangents cover glTF CUBICSPLINE
  (`RotateTracksComponent.h`). `MeshSystem.cpp:5201-5447` builds these entities on load.
  `ModelComponent.animations` serializes them as entity ids (`editor/Stream.cpp:6186-6245`), so
  every scene carries the generated track entities.
- **Blending.** `ActionSystem::update` (`ActionSystem.cpp:1915-1967`) runs single-threaded. It
  walks every `AnimationComponent` and then every `ActionComponent`. The weighted TRS sums per
  target are flushed in `flushPoseBlend` (`:1471-1520`, a normalised average). That supports N-way
  crossfades (`Model::playAnimation(name, fade)`, `object/Model.h:35-43`) and nothing else: no
  masks, additive layers, state machine, blend trees, IK, root motion, events or retargeting.
  `animationUpdate` still has `//TODO: Support loop actions` (`:466`).
- **Sampling cost.** `keyframeUpdate` (`:1307-1345`) finds the key by a linear scan from index 0
  for every track every frame, so it is O(keys). The accumulators are `std::unordered_map<Entity,
  …>` rebuilt each frame (`ActionSystem.h:61-62`).
- **Skinning.** The main thread recomputes `bonesMatrix` from the joint entities'
  `Transform::modelMatrix` for each `ModelSkinBinding` (`RenderSystem.cpp:12060-12094`, one
  `findComponent` per joint). `updateMeshBuffers` uploads the result to a uniform buffer or bones
  texture (`:7156-7164`). `MAX_BONES` is 128 (`Engine.h:106-107`).
- **API.** Lua exposes `Model:getAnimation/findAnimation/playAnimation/stopAnimations/getBone`
  (`GameObjectClassesLua.cpp:919-929`, `docs/mcp/guide/lua-api.md:761`). Action events are
  `onStart/onPause/onStop/onStep` (`lua-api.md:177`). MCP has `edit_animation`, `get_animation`,
  `preview_animation` and `set_sprite_animation` (`editor/mcp/tools/WorldTools.cpp:4305-4380`),
  plus entity types `animation` and `sprite_animation` (`EntityTools.cpp:220-221`).
- **Editor.** `editor/window/AnimationWindow.cpp` (3,124 LOC) is a track and action-frame timeline
  with keying and crossfade preview. It has **no curve editor**: cubic tangents are only kept
  mirrored when keys are written (`:543-600`).
- **Runtime data.** Exported games get their components as generated C++
  (`editor/Factory.cpp`, for example `createSpriteAnimationComponent` at `:2414`). The engine
  runtime has no YAML parser (only nlohmann JSON in `engine/libs/json`).
- **Frame order.** `Scene::init` registers ActionSystem → MeshSystem → UI → RenderSystem →
  PhysicsSystem → Audio (`Scene.cpp:34-40`). The engine loop runs fixed steps (physics) →
  `onUpdate` (Lua) → scene update → `onPostUpdate` (`Engine.cpp:1272-1300`). Raycasts go through
  `PhysicsLayers::raycast(Scene*, Ray, mask)` (`registry/PhysicsLayers.h:94`).
- **2D.** No 2D skeletal animation exists (no Bone2D or skeleton type anywhere). 2D motion is
  sprite flipbooks plus tweens.

## 4. Reference (OpenEngine, MIT)

Root: `external/OpenEngine/Engine/Modules/Animation/`. Their files carry no per-file copyright
header. Copied files get `// Adapted from OpenEngine (MIT), (c) 2026 Gamecrafters Guild Limited
and contributors` plus a `NOTICE.md` entry.

| File(s) | Verdict | Notes (verified by reading) |
|---|---|---|
| `Include/Animation/AnimationEvent.h`, `Source/AnimationEvent.cpp` (143) | **Copy** | `PlaybackStep` Forward/Wrapped/WholeLap/Backward, `CollectCrossedEvents`, polled collector. **Fix:** `Backward` "never wraps", so a looping clip at negative speed loses events after the wrap. Add `BackwardWrapped` and `BackwardWholeLap`. Port `Tests/AnimationCoreTests.cpp` event cases. |
| `docs/animation/gltf-clip-schema.mdx` | **Adopt as spec** | Field set, frame→time rule, end-frame tolerance, refusal rules, 256 KiB/16-depth bounds. **Improve:** they store `rootMotion` and `translations: restRelative` without reading them; we honour both. |
| `Source/HumanoidNameMatcher.cpp` (418), `Tests/HumanoidNameMatcherTests.cpp` | **Copy + adapt** | End-anchored token table covering Mixamo, Synty, MetaHuman, Maya and VRM names. Retarget to our `HumanBone`. |
| `Source/VRM1Importer.cpp`, `KHRHumanoidShared.cpp`, `KHRHumanoidImporter.cpp` | **Adapt** | Read `VRMC_vrm` (VRM 1.0) and `VRM` (0.x) (`VRM1Importer.cpp:70-71`), and `KHR_humanoid` (`KHRHumanoidImporter.cpp:109`, a draft). Rewrite them on tinygltf; they use cgltf. |
| `Source/BoneMask.cpp`, `Nodes/LayeredBlendNode.cpp`, `Nodes/AdditiveBlendNode.cpp` | **Adapt maths** | The per-bone mask and descendant fill need a topological order. Their additive blend expects a pre-made delta pose, and nothing generates one. Our import makes the deltas. |
| `Nodes/TwoBoneIKNode.cpp`, `FABRIKNode.cpp`, `LookAtNode.cpp`, `Source/PoseUtilities.cpp`, `Ops/FootLockOp.cpp`, `Tests/IKTests.cpp` | **Adapt maths, rewrite nodes** | The closed-form two-bone solve is useful. Its static path ignores the pole (`TwoBoneIKNode.cpp:225-226`). `ComputeWorldSpace` drops scale (`PoseUtilities.cpp`). `FootLockOp` walks the parent chain per bone with `chain[64]`. Fix all three. |
| `Nodes/BlendSpace1DNode.cpp`, `BlendSpace2DNode.cpp` | **Reference only** | 2D uses inverse-distance weighting and **reads only the first 16 samples** (`inputCount = min(size, 16)`), so the rest are silently ignored. We implement gradient-band interpolation (D6). |
| `Nodes/StateMachineNode.cpp`, `AnimationController.{h,cpp}` | **Field checklist only** | No exit time, any-state or interruption, and triggers are never consumed. Each transition evaluation allocates two `AnimationPose`s. The controller plays only its entry state (`Engine/Source/Engine/Rendering/AnimatorSourceResolution.cpp:145-168`). |
| `AnimationMontage.h`, `Source/MontageInstance.cpp` | **Reference only** | A stub. Its clip player is a placeholder ("clip-asset wiring is future work"), `NextSection` is never followed, and blend-out keys off the whole clip, not the section. |
| `Source/RootMotionExtractor.cpp` | **Do not copy** | Always bone 0. It takes a frame-to-frame delta of the *blended* pose, so a loop wrap jumps backwards, hence their 20 m/s teleport guard (`AnimationGraphSystem.cpp:80-96`). |
| `RetargetNode.cpp`, `HumanoidRig*`, `RetargetAssetWatcher` | **Do not copy** | XL, and cross-profile axis frames are still treated as identity (`RetargetNode.h:38`). |

## 5. Requirements

### Functional — runtime core
- **R1 Skeleton.** A `Skeleton` is built from a glTF skin (the union of a model's skins, plus the
  animated non-joint nodes) or from an entity hierarchy (generic or 2D rig). Bones are sorted
  parents-first. Each has a name hash, a parent index and a rest local TRS. Skeletons are shared
  per source asset.
- **R2 Clips.** An `AnimationClip` holds:
  - bone tracks (T, R, S) with LINEAR, STEP and CUBICSPLINE sampling;
  - morph-weight tracks;
  - property curves (`Curve`, bound by `path/Component.field` via S08);
  - discrete tracks (int, bool, sprite frame);
  - an event track;
  - settings: loop, speed, root motion (bone, translation, rotation, bake flags) and the additive
    reference.

  Sampling uses a per-instance cursor and is amortised O(1) per track. A seek falls back to binary
  search. Tracks bind to skeleton bones by name, and the binding table is cached per
  (skeleton, clip) pair.
- **R3 Pose operations.** These are local-space and SIMD (D3): blend two poses, accumulate N with
  normalisation, override with a per-bone mask, apply an additive with mask and weight, and
  convert local to model space with scale.
- **R4 Animator Controller.** An asset with:
  - parameters (Float, Int, Bool, Trigger);
  - layers (name, weight, Override/Additive, optional mask, IK pass, slot name);
  - nested state machines with states, sub-state machines, Entry, Exit and Any State;
  - transitions with the Unity semantics in D5: conditions, exit time, fixed or normalised
    duration, offset, interruption source and ordered interruption, can-transition-to-self.

  A state has a motion (a clip or a blend tree), speed with an optional multiplier parameter, cycle
  offset, an optional motion-time parameter, loop override (inherit/on/off) and a tag.
- **R5 Blend trees.** 1D, 2D Simple Directional, 2D Freeform Directional, 2D Freeform Cartesian
  and Direct, nestable, with ≤ 32 children per tree. Weights are non-negative and sum to 1 (Direct
  excepted). Children are phase-synced by normalised time, and the tree length is the
  weight-averaged child length.
- **R6 Masks and additive.** A `.mask` asset has humanoid body-part toggles and per-bone weights
  in 0..1. Additive clips are converted at bind time to deltas against their reference (frame 0,
  another clip's frame, or the rest pose). An additive layer at weight 0 leaves the pose
  bit-identical.
- **R7 Montages.** A `.montage` asset has a clip, slot name, sections (name, start, end, next
  section), blend in and out (seconds plus ease), play rate and its own event track. Playing a
  montage on a slot overrides that slot's layer output, using the layer's mask. A second montage
  on the same slot blends the first out. Sections chain; jump-to-section and set-next-section work.
  It reports `ended(interrupted)` and `blendingOut`.
- **R8 Events.** The ported crossing logic fires each event exactly once per pass in the play
  direction across forward, wrapped, whole-lap, backward and backward-wrapped steps. A seek
  through `play(state, t)` fires nothing. Events fire once **per Animator**, however many skinned
  meshes or models it drives. Firing rule (per Animator): `LeaderOnly` (default: per layer, only
  the highest-weight contributing clip fires) or `All` (every clip with weight > 0, weight passed
  along). Montage event tracks always fire.
- **R9 Root motion.** The bone is configurable: clip setting > avatar Hips > skeleton root. Per
  clip, the delta is sampled between the previous and current time, splitting at wraps. It is
  never a frame-to-frame delta of the blended pose. Deltas are blended by effective clip weight.
  The bake-into-pose flags (rotation Y, position Y, position XZ) keep those parts in the pose.
  `rootMotionMode` is Off, FromEverything or FromMontagesOnly. The output is applied to the
  Transform, handed to an S22 character controller when one is present, or delivered only to
  `onAnimatorMove` when a handler exists.
- **R10 IK stack.** It runs after layers and montages, in model space. The constraints are:
  - TwoBone, with a pole target (entity or position) or "keep bend";
  - FABRIK, a chain of 2–16 joints, 10 iterations by default, 1 mm tolerance, twist kept;
  - LookAt, up to 4 bones with per-bone weights and a clamp angle;
  - FootIK: a per-foot raycast through `PhysicsLayers::raycast`, a pelvis drop, ankle alignment
    to the surface normal (clamped) and spring smoothing;
  - FootLock: locks while the clip's contact curve is ≥ 0.5, or when an automatic velocity
    threshold is met, and releases over 0.1 s.

  Each weight and target can be bound to a parameter, a curve or Lua.
- **R11 Humanoid.** `HumanBone` is the VRM 1.0 humanBones set (55 bones, Unity aliases
  accepted). Auto-mapping takes the first source that works, in this order: `VRMC_vrm`, `VRM`
  0.x, `KHR_humanoid`, the name matcher. A rig counts as Humanoid when the required bones are all
  mapped: hips, spine, head, both upper and lower arms, both hands, both upper and lower legs and
  both feet. Otherwise it is Generic. A clip with a humanoid source plays on any humanoid target
  through D12. Generic clips bind by bone name only.
- **R12 Simple mode.** An Animator with no controller and a clip list runs as an implicit
  controller (one layer, one state per clip). `Model::playAnimation(indexOrName, fade)`,
  `stopAnimations` and `findAnimation` keep working on upgraded models by mapping to it.
- **R13 Other channels.** Morph-weight tracks write `MeshComponent::morphWeights` on the driven
  meshes. Sprite-frame tracks set the Sprite's frame through the existing
  `ActionSystem::setSpriteTextureRect` logic, which moves to a shared helper. Property curves
  write reflected fields. Tweens, `SpriteAnimationComponent` and legacy `Animation` entities work
  unchanged. When a legacy track targets an Animator-driven bone, the Animator wins and a one-time
  warning names both.
- **R14 Overrides and sharing.** `clipOverrides` (clip → clip) reuses one controller with
  different clips. `drivenModels` lists extra Models (armour, heads) that share the skeleton by
  bone name. The Animator drives their skins from its pose.
- **R15 Modes.** The update modes are Normal, AnimatePhysics (evaluated in `fixedUpdate` before
  physics) and UnscaledTime. The culling modes are AlwaysAnimate, CullUpdateTransforms (time,
  state machine, events and root motion advance, but sampling, IK and skinning are skipped when
  no driven renderer was visible to any camera last frame) and CullCompletely. Optional rate LOD
  by screen height evaluates at ½ or ¼ rate.
- **R16 Bone sync.** `All` writes local TRS to every bone entity, the legacy behaviour. `Exposed`
  writes only the listed bones and the bones with children that are not bones (attachments).
  `None` writes no bone entities. Skin matrices never depend on bone entities (D7).

### Platform & backends
- **R17** Everything runs on GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan, and on all six
  export targets. No compute and no SSBO requirement. GPU skinning, `MAX_BONES` and the
  bones-texture fallback are unchanged.
- **R18** SIMD uses S20's `engine/core/math/simd/SimdFloat4.h` (SSE2, NEON, wasm simd128,
  scalar). The scalar build is always compiled and tested. SIMD results match scalar within
  1e-5 per component.
- **R19** With S06 worker count 0 (web without pthreads), the system runs serially with
  bit-identical results.

### Performance (S06 reference machine: Linux x64, Release, 8 workers)
- **R20** `AnimBench.Crowd200`:
  - Rig: a 65-bone humanoid with `boneSync: Exposed` (4 exposed bones) and every character
    visible.
  - Workload per character: base layer is a Freeform-Cartesian tree with 5 clips (3–4 active)
    plus a crossfade on 25 % of characters; an upper-body override layer with a mask; an additive
    layer; FootIK ×2 (raycasts included); LookAt over 3 bones; root motion; events.
  - Desktop: AnimatorSystem wall time ≤ 1.5 ms p95, main-thread share ≤ 0.35 ms p95, and 0 heap
    allocations per frame after warm-up.
  - Serial (0 workers): ≤ 8 ms.
  - Mobile reference (Android arm64, 4 workers, documented device): ≤ 6 ms p95, which fits
    30 fps.
- **R21** Micro budgets:
  - two-pose blend at 128 bones ≤ 1 µs, with SIMD ≥ 2× faster than scalar;
  - one 65-bone clip sample ≤ 2 µs;
  - state-machine update with 20 states and 60 transitions ≤ 1 µs;
  - controller instance runtime state ≤ 8 KB.
- **R22** Skin matrices for 200 × 65 bones are produced inside the parallel wave. RenderSystem's
  per-joint loop no longer runs for Animator-driven skins.

### Editor UX (human)
- **R23 Animator window** (Window > Animation > Animator):
  - Left tabs Layers and Parameters: add, rename, reorder and delete; layer weight, blending, mask
    and IK pass; parameter defaults.
  - A graph canvas (D14) with a breadcrumb for sub-state machines. Colours: Entry green, Any State
    cyan, Exit red, default state orange. States and sub-machines are boxes, transitions are arrows
    (paired arrows offset, a multiplicity mark when there are several).
  - Right-click menus: Create State (Empty / From Clip / From New Blend Tree), Make Transition,
    Set as Layer Default.
  - Dragging a clip from the Project window creates a state.
  - Selecting a state or transition shows it in the Inspector. The transition inspector has the
    conditions list and a scrub bar previewing the blend on the selected model.
  - Every edit is one named undo step (S24).
- **R24 Live debugging.** In Play, the window follows the selected Animator (or a pinned one). It
  shows the active state's normalised-time progress bar, the transition progress on its arrow,
  live parameter values (editable, triggers as buttons), layer weights, the playing montage and
  section, and the IK targets drawn in the Scene view.
- **R25 Blend tree editor.**
  - The Inspector holds the child list (motion, threshold or position, time scale) and an
    "Automate thresholds" or "Compute positions from root-motion velocity" action.
  - 1D shows the weight curves.
  - 2D shows the sample dots, the draggable parameter point and the weight-sized rings.
  - A preview pane plays the result on the model.
- **R26 Avatar and mask editors.**
  - The model Inspector has a Rig tab: Generic or Humanoid, and the root-motion bone.
  - Configure Avatar shows the bone map grouped Body / Head / Left Hand / Right Hand, with a
    clickable figure diagram and green/red status. It has Auto-Map, Clear and Enforce T-Pose, and
    selecting a row highlights the bone in the Scene view.
  - The mask editor has a body-part figure with toggles and a bone tree with checkboxes and
    weights.
- **R27 Clip editing** in the Animation window, for `.anim` clips and Animator-driven models:
  - Dope sheet, plus a Curves tab using the shared curve widget (D15): tangent menu, extrapolation
    menus, frame-all and frame-selection, box select, multi-curve edit, value and time snapping.
  - An event lane: add, move, delete, and edit name and payload in a popup.
  - A record mode that keys scene changes to bones and properties into the clip.
  - A montage section lane when a `.montage` is open.

  Imported clips are read-only except settings and events, which are saved in the model meta. A
  "Duplicate as editable clip" action is available. The legacy timeline mode stays for
  `Animation` entities.

### Agent UX (MCP, Lua, CLI)
- **R28 Lua.** The `Animator` wrapper, `AnimatorComponent` events and enums from D16 are
  documented in `lua-api.md` (new §4.x) and in the S23 API JSON, with a mapping to Unity names in
  `docs/unity-glossary.md`.
- **R29 MCP.** The tools in D17 are registered in `editor/mcp/tools/AnimationTools.cpp`, with the
  resource `jasper://animator/{entity}`. Every editing tool is one undo step and validates before
  it writes. The existing four tools keep their behaviour on legacy timelines, and `get_animation`
  also reports Animator clips. `docs/mcp/TOOLS.md` and the agent guide get an "Animate a
  character" recipe.
- **R30 CLI.** All tools are reachable through S31's `jasperengine-editor call`.

### Data & compatibility
- **R31** New YAML assets `.animator`, `.anim`, `.mask` and `.montage`. Each has a top-level
  `guid:` (S04 R2), `formatVersion: 1` and a migration table (S03 R21), and preserves unknown
  keys (S03).
- **R32** Settings and events for imported glTF clips, and the humanoid avatar, live in the model
  meta under `import.animations.<subAssetKey>` and `import.avatar` (S04/S14 meta container).
  Precedence, lowest to highest: glTF `extras.clip` < meta < state override.
- **R33** Old scenes load and play bit-identically. A `ModelComponent` without the new
  `animationMode` key decodes as `Legacy`. Models created after this spec write
  `animationMode: animator` and generate no track entities. No scene version bump is needed
  (S03 R22: additive field). **Upgrade to Animator** (per model, or project-wide with a report)
  creates an `AnimatorComponent` in simple mode, compares each generated track with its glTF
  source, writes `.anim` overrides for edited clips, and removes the generated entities, all in
  one undo step.
- **R34** The editor cooks the four YAML assets and the meta clip blocks to the binary format
  (D13) for Play and export (S30). Factory codegen emits only `AnimatorComponent` fields and
  asset refs.

## 6. Design constraints & decisions

- **D1 Layout.** Namespace `jasperengine::anim` in `engine/core/animation/`:
  - `Skeleton`, `Pose`, `PoseOps`, `Clip`, `ClipSampler`, `ControllerDef`, `AnimatorInstance`,
    `StateMachine`, `BlendTree`, `Mask`, `Montage`;
  - `ik/{TwoBone,Fabrik,LookAt,FootIK,FootLock}`, `RootMotion`, `Events` (port);
  - `humanoid/{HumanBone,NameMatcher,Avatar,Retarget,VrmImport}`, `AnimCodec`.

  Component: `component/AnimatorComponent.h`. System: `subsystem/AnimatorSystem.{h,cpp}`,
  registered after ActionSystem through an S07 `SystemDescriptor` (phase Update; FixedUpdate for
  AnimatePhysics). The runtime is not built on S23's generic graph model: states, transitions and
  trees are typed, because a dataflow model adds indirection and gives no runtime gain. The
  editor-side graph widget is shareable (D14).
- **D2 Pose.** SoA `float` arrays: `tx ty tz`, `qx qy qz qw`, `sx sy sz`, and `channels[]`
  (morphs and properties). Bones are padded to a multiple of 4 and aligned to 16 bytes. Buffers
  come from S06 `ScratchScope`. Persistent per-instance state (cursors, transition snapshot,
  IK springs, foot locks) lives in one block per instance, sized at bind.
- **D3 Blend maths.** N-way rotation: hemisphere-align to the first contributor, weighted sum,
  normalise (nlerp). Two-way also nlerps. Additive: `q = base · nlerp(I, Δ, w·m)`, `t += Δt·w·m`,
  `s *= lerp(1, Δs, w·m)`. Override layer: per-bone `lerp(base, layer, w·m)`. Model space keeps
  scale (fixes their `ComputeWorldSpace`). Compile `animation/` with `-ffp-contract=off`, as S20
  does.
- **D4 Sampling.** Tracks that share key times (glTF samplers usually do) are grouped and share
  one cursor. Uniformly keyed groups use `index = floor(t·rate)`. Optional import keyframe
  reduction: rotation 0.5°, position and scale 0.5 %. Off by default; a per-clip meta setting.
- **D5 Transition semantics (Unity Mecanim).**
  - Any State transitions are checked first, then the current state's, in list order.
  - At most one transition starts per layer per update.
  - Conditions are ANDed:
    - Float: Greater, Less;
    - Int: Greater, Less, Equals, NotEqual;
    - Bool: If, IfNot;
    - Trigger: If.

    Triggers stay set until a transition consumes them or `resetTrigger` clears them.
  - `exitTime < 1` is tested on every loop at that fraction; `≥ 1` fires once.
  - A transition with exit time and no conditions fires on exit time alone.
  - Interruption source is None, Current, Next, CurrentThenNext or NextThenCurrent. During a
    transition only those transitions are tested. `orderedInterruption` stops at the active
    transition's index. An interrupted crossfade freezes the in-flight blend as a snapshot pose
    (one extra pose, no recursion).
  - Entering a sub-state machine follows its Entry transitions, then its default state. Exit
    returns to the parent machine's transitions out of that sub-machine node.
  - `setFloat(name, v, damp, dt)` does critically damped smoothing.
- **D6 Blend-tree weights.**
  - 1D: piecewise linear between neighbours.
  - Simple Directional: barycentric over the angular neighbours plus the centre sample.
  - Freeform Cartesian and Directional: gradient-band interpolation (Johansen 2009; Directional in
    polar space). The per-tree pair data is precomputed at compile time. Evaluation is O(n²) with
    n ≤ 32 and allocation-free.
  - Weights below 1e-3 are dropped and the rest renormalised.
- **D7 Execution.**
  1. **Prepare** (main thread): apply queued Lua parameter writes and IK targets, run the FootIK
     raycasts from last frame's foot positions (one frame of latency, documented), and resolve
     culling from last frame's visibility.
  2. **Wave** (`jobs::parallelFor`, 4 animators per chunk): advance the state machines; sample,
     blend, apply montages, IK and root-motion extraction; compute model space; write
     `bonesMatrix = inv(meshNodeModel) · jointModel · IBM`. This is model-relative and needs no
     entity world matrices. Then write morph weights and the synced bone local TRS
     (`needUpdate`). The Animator owns every entity it writes. When two Animators claim one
     entity, the second gets a bind error and drives nothing.
  3. **Finalize** (main thread): apply root motion, property and sprite tracks; deliver state
     changes, montage notifications and events in entity order to C++ subscribers and then to Lua.

  `ModelSkinBinding` gains `animatorDriven`, and `RenderSystem.cpp:12060-12094` skips bindings
  that have it. Lua never runs on workers.
- **D8 Defaults.** New Animator: Normal update, CullUpdateTransforms, `boneSync: Exposed`, rate
  LOD off, `LeaderOnly` events, FromEverything root motion when `applyRootMotion` is on (off by
  default). Upgraded models get AlwaysAnimate and `boneSync: All`, to keep behaviour.
- **D9 Root motion → S22.** The Animator publishes `deltaPosition` (world), `deltaRotation` and
  `velocity = Δ/dt` each update. When an S22 character controller is on the entity, S22 consumes
  that velocity at its next fixed steps, with collisions. Otherwise the Transform gets the delta
  in Finalize. A per-frame delta above 50 m is dropped with a warning. This is a safety net, not
  the teleport guard OpenEngine needs.
- **D10 Events.** The ported collector runs per Animator per frame. Payload is
  `variant<monostate, float, int32, bool, string>`. C++ gets
  `FunctionSubscribe<void(const AnimEvent&)> onEvent` (S09 RAII). Lua gets `RegisterEvent(...)`
  with handler args `(name, payload, clipName, weight)`. When `sendToScripts` is set (default on),
  a script function named like the event on the Animator's entity is also called with
  `(payload)`, as Unity does.
- **D11 IK space.** Solvers run on a model-space cache (position, rotation, scale) and write back
  local rotations parents-first. Two-bone uses law-of-cosines with the pole projected onto the
  chain plane and the target clamped to 0.999 of the chain length. FABRIK reconstructs rotations
  by shortest-arc per joint and keeps each joint's twist. FootIK smoothing is a critically damped
  spring with a 0.08 s half-life.
- **D12 Retarget.** For a mapped bone with source and target reference (T-pose) world rotations
  `Rs`, `Rt`: `qt_world = qs_world · Rs⁻¹ · Rt`, then converted to local. The per-bone pre and
  post corrections are precomputed at bind, so the cost is 2 quaternion multiplies per bone.
  Hips translation is scaled by `hipHeight_t / hipHeight_s`. Other bones keep the target's local
  translation. Reference poses come from the bind pose, or from the avatar's "Enforce T-Pose"
  corrections. This removes OpenEngine's identity-axis-frame limitation.
- **D13 Cooked format.** The binary format is a little-endian `JANM` container: header, version,
  sections (skeleton refs, curves, tracks, controller, masks, montage) and a CRC32. It is written
  to `.jasper/cache/anim/<guid>.bin` and copied by export. The runtime reader bounds-checks every
  offset and refuses unknown major versions. Codecs live in `AnimCodec` (engine) and
  `editor/animation/AnimYaml.{h,cpp}` (YAML ↔ definitions).
- **D14 Graph canvas.** We vendor neither node-editor library:
  - **imnodes** (MIT) has pin-to-pin links only, and no zoom on master.
  - **imgui-node-editor** (MIT) zooms, but it depends on ImGui internals, and its last tagged
    release predates the 1.92 font and texture rework we ship (`libs/imgui/imgui.h:32`, 1.92.8).
  - State machines need node-to-node arrows anyway.

  We build `editor/widget/GraphCanvas.{h,cpp}` (about 1k LOC): pan and zoom (1.92 dynamic font
  sizes keep text crisp), boxes, straight arrows with offset pairs, box select, grid snap and a
  hook for selection and undo. S23 may reuse it.
- **D15 Curve.** S21 lands `engine/core/math/Curve.h` first (S20 D5 defers to it):
  - Keys: `{time, value, inTangent, outTangent, inWeight, outWeight, mode}`.
  - Tangent modes per side: Auto, ClampedAuto, Free, Linear, Constant, plus a broken flag.
  - Weighted keys are cubic Bézier, solved by Newton (≤ 8 iterations, 1e-6). Unweighted keys are
    Hermite.
  - Pre and post wrap: Constant, Linear, Loop, PingPong, CycleWithOffset.

  `editor/widget/CurveEditor.{h,cpp}` is the shared widget (multi-curve, tangent handles,
  wrap-region ghosting, pinned reference curves). S27 adds presets and the library. `.anim` bone
  rotations are stored as `euler` (3 curves, the default for editor-made tracks) or `quaternion`
  (glTF duplicates; dope-sheet editable, curves view read-only).
- **D16 Lua API** (camelCase, the engine convention):
  ```lua
  local a = Animator(self.scene, self.entity)
  a:setFloat("Speed", v [, damp, dt]); a:setInteger(n, i); a:setBool(n, b)
  a:setTrigger(n); a:resetTrigger(n); a:getFloat(n) -- also getInteger/getBool, getParameterId(n)
  a:play("Base Layer.Run" [, layer, normalizedTime]); a:crossFade(state, dur [, layer, offset])
  a:crossFadeInFixedTime(state, seconds [, layer]); a:playClip(name [, fade])  -- simple mode
  a:getCurrentState(layer) -- {name, fullPath, tag, normalizedTime, length, loop}
  a:isInTransition(layer); a:setLayerWeight(i, w); a:getLayerIndex(name)
  a:playMontage(path [, rate, section]); a:stopMontage([blendOut]); a:isMontagePlaying()
  a:montageJumpToSection(n); a:montageSetNextSection(from, to)
  a:setIKTarget(name, pos [, rot, weight]); a:setIKWeight(name, w); a:setLookAt(pos [, weight])
  a:getBoneWorldTransform(nameOrHumanBone) -- position, rotation from the evaluated pose
  a.speed; a.applyRootMotion; a:getRootMotion() -- deltaPosition, deltaRotation
  local c = a:getAnimatorComponent()  -- events: onEvent(name,payload,clip,weight),
  -- onStateEnter(layer,state), onStateExit(layer,state), onMontageEnded(montage,interrupted),
  -- onAnimatorMove(deltaPos,deltaRot): when handled, root motion is not auto-applied
  ```
  Calls made on a non-Play scene work in editor preview and are documented. An unknown parameter
  or state logs once per name per Animator and is otherwise a no-op.
- **D17 MCP tools.**
  - `get_animator_controller {path}` returns the whole controller plus diagnostics (unreachable
    states, unknown parameters, missing clips).
  - `edit_animator_controller {path, create?, ops[]}`. Ops: `add/remove/rename_parameter`,
    `add/set/remove_layer`, `add/set/remove_state`, `add_sub_state_machine`,
    `add/set/remove_transition` (from: state, `AnyState` or `Entry`; to: state or `Exit`),
    `set_default_state`, `set_blend_tree`.
  - `set_animator {entity, controller?, avatar?, clips?, overrides?, apply_root_motion?,
    update_mode?, culling_mode?, bone_sync?, ik?}`.
  - `get_animator_state {entity}`: the definition summary in Edit mode, live layers, states,
    transition, parameters, montage and IK in Play.
  - `set_animator_parameters {entity, values, triggers}`, `animator_play {entity, state, layer?,
    crossfade?, normalized_time?}` and `play_montage {entity, montage, rate?, section?}`, all
    Play only.
  - `get_animation_clip {clip}` and `edit_animation_clip {clip, create?, ops[]}`. Ops: set or
    remove keys, tangents, wrap, events, settings. Imported clips accept only events and settings.
  - `get_avatar {model}` and `edit_avatar {model, auto_map?, mapping?, enforce_t_pose?}`.
  - `edit_avatar_mask {path, create?, body_parts?, bones?}` and `edit_montage {path, create?,
    clip?, slot?, sections?, blend?, events?}`.
  - `sample_animation_pose {entity, state?, params?, time?}`: bone world TRS of an evaluated
    pose, for agents to verify.
  - `upgrade_model_animation {entity | all, dry_run?}`.
- **D18 Controller YAML** (abridged; clip refs are S04 GUIDs with a path hint):
  ```yaml
  guid: 7b1e...                # S04
  formatVersion: 1
  parameters: [{name: Speed, type: float, default: 0}, {name: Jump, type: trigger}]
  layers:
    - {name: Base Layer, weight: 1, blending: override, mask: null, slot: Default, ikPass: true,
       stateMachine: {defaultState: Locomotion,
         anyState: [{to: Hit, conditions: [{param: Hit, mode: if}], duration: 0.1}],
         states: [{name: Locomotion, position: [240, 120], speed: 1, tag: "",
           motion: {blendTree: {type: freeformCartesian2D, paramX: VelX, paramY: VelZ,
             children: [{clip: {guid: 21ab..., hint: "Knight.glb#animation:Run"}, pos: [0, 1]}]}},
           transitions: [{to: Jump, conditions: [{param: Jump, mode: if}], hasExitTime: false,
             exitTime: 0.9, fixedDuration: true, duration: 0.15, offset: 0,
             interruptionSource: none, orderedInterruption: true}]}],
         subStateMachines: []}}
  ```
- **D19 Delivery order.**
  - M1: core (pose, sampler, events, simple mode, upgrade, skinning path).
  - M2: controller, blend trees, layers, masks.
  - M3: Animator window and curve editor.
  - M4: IK and root motion with S22.
  - M5: montages and humanoid retargeting.

  Each milestone merges green on its own.

## 7. Acceptance criteria

Tests live in `engine/tests/core/animation/` and `editor/tests/animation/` (S02 layout). MCP tests
go in `tools/mcp/tests/test_animator.py`. Fixtures are a generated CC0 65-bone humanoid with
6 clips, a second humanoid with different bone axes and names, a 2D cut-out rig scene, and the
existing glTF samples.

- **AC1 (R8)** `AnimEvents.*` (ported from `AnimationCoreTests.cpp` plus new cases): Forward,
  Wrapped, WholeLap, Backward and BackwardWrapped each fire every crossed event once, in order;
  dt = 3 laps fires each event once; an event at the end frame fires on arrival; a seek fires
  none. `AnimatorSystem.EventsOncePerAnimator`: one Animator driving 2 models and 3 skinned
  meshes fires a `hit` event exactly once per pass over 300 frames.
- **AC2 (R5, D6)** `BlendTree.*`:
  - weights are ≥ 0 and sum to 1 ± 1e-6 over a 64×64 parameter grid for each 2D type;
  - a parameter exactly on a sample gives that sample weight 1;
  - a 1D tree is exact at its thresholds;
  - a 20-sample tree uses every sample (a regression test for OpenEngine's 16-sample cut);
  - after 10 s of blended playback the children's normalised-time drift is ≤ 1e-4.
- **AC3 (R4, D5)** `StateMachine.*`:
  - `ExitTimeEachLoop` (0.75: fires at 0.75 of the current loop ± 1 frame);
  - `ExitTimeOnce` (1.5);
  - `AnyStateFirst`;
  - `TriggerConsumedOnce` (one trigger, two eligible transitions: one taken, trigger cleared);
  - `TriggerPersistsUntilUsed`;
  - `InterruptionNextThenCurrent`;
  - `OrderedInterruption`;
  - `SubStateEntryExit`;
  - `NoTransitionToSelfUnlessAllowed`;
  - `InterruptedBlendIsContinuous`: the max bone jump across an interruption is ≤ that of an
    uninterrupted frame step.
- **AC4 (R3, R6, D3)** `PoseOps.SimdMatchesScalar` (1e-5, 10k random poses) and
  `Layers.MaskOverride` (masked bones exactly the layer pose, unmasked bones exactly the base).
  `Additive.ZeroWeightBitIdentical`. `Additive.ReferenceFrame0`: the additive clip at its
  reference frame changes nothing.
- **AC5 (R10, D11)** `IK.*`:
  - TwoBone reaches a reachable target within 1 mm, and the mid joint lies on the pole side of
    the chain plane;
  - an unreachable target gives a straight chain aimed at it;
  - FABRIK converges within 10 iterations to 1 mm on 8 random reachable cases;
  - LookAt never exceeds its clamp angle;
  - non-uniform-scale chains are correct (the scale fix).
- **AC6 (R10)** `FootIK.SlopeGrounding` (scene fixture with a 20° ramp and a step): both soles
  are within 1 cm of the surface after 30 frames, and the pelvis never rises above the
  animation's. `FootLock.NoSlide`: a locked foot drifts ≤ 2 mm in world space during contact.
- **AC7 (R9, D9)** `RootMotion.LoopWrapContinuous`:
  - 10 laps of a looping walk accumulate 10 × the clip displacement within 0.1 %, with no
    backward frame;
  - `ConfigurableBone`: a clip declaring `rootMotion.bone: "root"` uses that bone, not bone 0;
  - `BakeIntoPoseY` keeps Y in the pose.

  An MCP test with S22 shows a controller-driven character following root motion up a ramp,
  without tunnelling.
- **AC8 (R11, D12)** `NameMatcher.*` (ported cases) pass. `Retarget.SameRigIdentity`: retargeting
  onto the same rig is within 1e-5 of direct playback. `Retarget.CrossAxisConvention`: a walk
  retargeted onto the second fixture keeps the hand and foot world directions within 5° of the
  source, scaled by hip height. `Avatar.VrmMapping`: a VRM 1.0 sample maps every required bone
  from the extension.
- **AC9 (R2, R32)** `GltfClipSchema.*`:
  - each refusal rule in the schema doc logs one warning, and the clip loads without settings;
  - frames convert to time per the doc's rate rule;
  - meta overrides beat extras;
  - `restRelative` translations sample relative to the rest pose.
- **AC10 (R33)** `LegacyAnimation.BitIdentical`: the S03 scene fixtures with animated models,
  played for 240 frames, give byte-identical bone `Transform`s and `bonesMatrix` before and after
  this spec. `Upgrade.*`: an unedited model upgrades with no `.anim` written, and its pose stays
  within 1e-5 of legacy for 240 frames; an edited track produces one `.anim` and the same pose;
  undo restores the original entities; `Model:playAnimation("Run", 0.2)` works after the upgrade.
- **AC11 (R31, D13)** `AnimYaml.RoundTrip` (encode → decode → encode is stable), unknown keys
  preserved, and `AnimCodec.FuzzNoCrash` (100k mutated blobs under ASan: refused or loaded, no
  OOB read).
- **AC12 (R20-R22)** `AnimBench.Crowd200` meets the R20 thresholds in the S02 benchmark job;
  `AnimBench.Micro*` meets R21; `AnimatorSystem.ZeroAllocSteadyState` (allocation counter, 600
  frames); `AnimatorSystem.ParallelEqualsSerial` (bit-identical poses, events and root motion over
  600 frames). The mobile figure is checked manually on the documented device before M2 merges,
  and the result is recorded in `docs/animation/perf.md`.
- **AC13 (R13, R15, R16)** `Channels.MorphAndSprite`: morph and sprite-frame tracks hit their
  values at key times. `Culling.CullUpdateTransforms`: an invisible Animator still fires events
  and advances state, and `bonesMatrix` stays unchanged. `BoneSync.Exposed`: only the exposed
  bones' `Transform`s change. A RenderGpu golden of the fixture humanoid at three poses (glcore
  and Vulkan, S02).
- **AC14 (R1, 2D)** `GenericRig.Cutout2D`: a 6-sprite cut-out rig in a 2D scene plays a
  controller with a blend tree and sprite-frame track, and its pose matches the expected pose at
  t = 0.5 s within 1e-5. An MCP test builds this rig through tools alone.
- **AC15 (R23-R27)** Editor tests (`editor/tests/animation/`): each Animator-window operation is a
  single named undo step that undoes exactly; the curve editor's tangent-mode changes reproduce
  Unity's ClampedAuto values on a reference curve (table in the test). Manual check, recorded in
  the PR: create a 3-state controller with a 2D blend tree by mouse only, enter Play, and watch the
  live state, transition progress and parameter edits.
- **AC16 (R28-R30)** `test_animator.py`, which uses only tools and Lua:
  - Builds a controller (idle, locomotion tree, jump, any-state hit), assigns it, enters Play and
    sets parameters.
  - Checks state transitions through `get_animator_state`, Lua `onEvent` handler counts, a
    montage with sections `A→B`, and `sample_animation_pose` bone positions.
  - Repeats one call through the S31 CLI.

  The Lua docs examples are executed by the S23 doc test. `tools/smoke-test.sh` stays green.

## 8. Dependencies & conflicts

- **Depends on:**
  - S01 and S02 (tests, benchmarks, goldens).
  - S03 (`formatVersion`, migrations, preservation).
  - S04 (GUIDs, `animation#n` and `skin#n` sub-assets, metas, refs).
  - S06 (`parallelFor`, `ScratchScope`). Serial until it lands.
  - S07 (`SystemDescriptor`, phases).
  - S08 (reflected fields for `AnimatorComponent` and property curves).
  - S09 (RAII subscriptions).
  - S14 (meta import blocks).
  - S20 (`SimdFloat4.h`; whoever lands first owns it).
- **Works with:**
  - S22: consumes `velocity` and `deltaRotation`; owns the controller side of AC7.
  - S05: hot reload of `.anim`, `.animator`, `.mask`, `.montage` and `.glb` rebinds live
    Animators, keeping their state when the state names still exist.
  - S23: API JSON and doc tests. It may reuse `GraphCanvas`.
  - S24: named undo, one timeline.
  - S27: extends `CurveEditor`.
  - S30: ships the cooked `.bin` files.
  - S31: CLI.
  - S15: profiler scopes `Anim.Prepare`, `Anim.Wave` and `Anim.Finalize`.
- **Blocks:** S20's and S27's curve UI (they consume D15). S22 root-motion locomotion.
- **Merge hotspots:**
  - `engine/core/subsystem/ActionSystem.cpp` (S20 also edits it; we only extract the sprite-rect
    helper);
  - `MeshSystem.cpp` glTF import (`:5196-5450`, skin bindings `:2332-2366`);
  - `RenderSystem.cpp:12060-12094`;
  - `editor/Stream.cpp` and `Factory.cpp`;
  - `editor/Catalog.h` `ComponentType` (append only, coordinate with S18, S19, S20, S22);
  - `GameObjectClassesLua.cpp`, `WorldTools.cpp` (tool registry), `AnimationWindow.cpp`;
  - `docs/mcp/guide/lua-api.md`, `docs/mcp/TOOLS.md`, `NOTICE.md`.

## 9. Risks & defaults

- **Two runtimes coexist** (legacy entity tracks and the Animator). This is the biggest risk:
  behaviour drift, and bugs fixed twice. Default: the legacy path is frozen, bug fixes only. New
  models default to the Animator. The upgrade is explicit and reported. Deprecating the legacy
  path is a later decision, once AC10 has held for a release.
- **Bone-entity sync dominates main-thread cost** with `boneSync: All`. Default for new
  Animators is `Exposed`, and R20 is measured with it. The cost of `All` is reported by the
  benchmark, not gated.
- **Retarget quality without muscle space** (twist and finger artefacts on very different rigs).
  Default: T-pose world-delta retargeting (D12) with Enforce T-Pose. Muscle space is a later
  spec if AC8 fixtures show it is needed.
- **FootIK raycasts have one frame of latency** and run on the main thread. Default: accept,
  smooth with the spring, and cap at 2 raycasts per character. A Jolt batched query is allowed
  later if Prepare exceeds 0.2 ms.
- **`KHR_humanoid` is a draft.** Default: read it if present and log the extension name. VRM and
  the name matcher are the reliable paths.
- **Web without threads** runs about 100 characters at 60 fps (the R20 serial budget). It is
  documented, and rate LOD is recommended for crowds.
- **Graph canvas effort** (about 1k LOC). We accept it to avoid pinning ImGui. If S23 lands a
  canvas first and it meets D14, use theirs.
- **glTF event frame-rate ambiguity** in clips with irregular keys. Default: the schema doc's
  rate rule, with refusal on an unknown rate. Events edited in the editor are stored in seconds in
  the meta.
- **Interrupting a transition** uses one frozen snapshot pose instead of nested blends (as
  Unity does). Rapid chains of interruptions therefore blend from the latest snapshot only.
