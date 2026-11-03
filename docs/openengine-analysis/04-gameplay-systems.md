# 04 — Gameplay runtime systems: animation, physics, audio, input, text, picking

Comparison of JasperEngine (OURS) and OpenEngine (THEIRS, MIT, `external/OpenEngine`). Everything below comes from reading the code; nothing was built or run.
Path prefixes: `OE/` = `external/OpenEngine/`. Unprefixed paths are in our repo.

---

## 1. What THEIRS has

### 1.1 Animation (`OE/Engine/Modules/Animation`, ~14k LOC excluding tests, ~7k LOC of tests)
Layout: a pose-based graph runtime. `AnimationPose` (`Include/Animation/AnimationPose.h`) is a set of SoA arrays (positions, rotations, scales, one entry per bone). It flows through `AnimGraphNode::Evaluate(EvaluationContext&, AnimationPose&)` (`AnimGraphNode.h`, `EvaluationContext.h`). The context carries the delta time, a param map, an event collector, the accumulated blend weight (`EvaluateAtWeight`) and the source/target skeleton and profile for retargeting.
- **Clips**: `AnimationClip` (header `Include/Animation/AnimationClip.h`, implementation `OE/Engine/Source/Assets/AnimationClip.cpp`, 1626 LOC). Channels cover T/R/S and morph weights, with interpolation Linear/Step/CubicSpline. Per-key tangent types are Auto/Plateau/Clamped/Fixed/Linear/Flat/Custom, with weights, broken tangents and a per-segment interpolation override. Pre/post extrapolation is Constant/Linear/Cycle/CycleWithOffset/Oscillate. There is an editable `.anim` format (load/save/edit API) and name-keyed channel binding (`targetNameId`).
- **glTF clip extras schema** (`OE/docs/animation/gltf-clip-schema.mdx`): `extras.clip` sets loop, speed, root-motion bone, translation mode and **events by frame**. The docs admit that `rootMotion.bone` and `restRelative` are stored but playback does not read them.
- **Graph nodes** (`Source/Nodes/*`): ClipPlayer, Blend2, **BlendSpace1D**, **BlendSpace2D**, **LayeredBlend** (per-bone `BoneMask`), **AdditiveBlend**, **StateMachine**, **MontageSlot**, **TwoBoneIK**, **FABRIK**, **LookAt**, **FootIK**, plus **RetargetNode** and an op stack (`Ops/FootLockOp`, `Ops/LookAtOp`, `Ops/AttachmentPassthroughOp`).
  - `BlendSpace2DNode.cpp` uses inverse-distance weighting over the K nearest samples, without allocation (inline array of 16). It does not triangulate like Unity's freeform or Delaunay modes, so it is cheaper but less accurate.
  - `StateMachineNode.cpp` has param conditions (`AnimParam.h`: float/int/bool variant, 6 comparison ops) and a linear crossfade. It has **no exit time, no any-state and no interruption rules**.
  - FootIKNode is **not wired** into the graph compiler: its `kType*` names are missing from `OE/Engine/Source/Engine/Rendering/Systems/AnimationGraphCompileFromModel.cpp`. It also needs the caller to raycast and pass in the ground results.
- **Montages**: `AnimationMontage.h` and `MontageInstance.cpp` give sections with next-section chaining, blend in/out, play rate, bone mask and an event track. `MontageSlotNode` overlays a montage on the graph.
- **Animation events**: `AnimationEvent.h/.cpp`. Each event has a time, a name and a variant payload. `PlaybackStep` handles Forward/Wrapped/WholeLap/Backward, so events fire exactly once across loops and when playing backward. `AnimationEventCollector` collects events per animator per frame, and scripts poll them with `MarkPolled`. Clip-playback claiming dedupes events when several skinned meshes share one animator. All of this depends only on the C++ standard library.
- **Root motion**: `RootMotionExtractor.cpp` takes the delta of bone **0** (always; the declared bone is ignored) and zeroes it. `OE/Engine/Source/Engine/Rendering/Systems/AnimationGraphSystem.cpp:80-96` applies it to the Transform, **or feeds it into `CharacterController.animationDisplacement*`**, with a teleport guard (`kMaxRootMotionSpeed` 20 m/s, minimum discontinuity 2 m).
- **Humanoid retargeting** (large; roughly 3.5k LOC across `HumanoidRig*`, `HumanoidNameMatcher.cpp` (418), `HumanoidImport.cpp` (632), `KHRHumanoidImporter`, `VRM1Importer`, `SkeletonProfile`, `RetargetMap`, `RetargetNode.cpp` (564), `RetargetAssetWatcher`): canonical `HumanBone` enum, auto-mapping by bone name, VRM1/KHR humanoid import, chain transport modes, translation scaled by hip height, foot locking. `RetargetNode.h` notes that axis frames are still treated as identity across profiles.
- **SkeletonModifierStack** (`SkeletonModifierStack.cpp`) only implements **CopyTransform**. LookAt/Aim/TwoBoneIK/FABRIK/CCDIK/SpringBone/Twist/PhysicalBone exist as enum stubs that do nothing.
- **AnimationController asset** (`AnimationController.h/.cpp`): Unity-like JSON with params (Float/Int/Bool/Trigger), states, 1D/2D blend trees, and transitions that carry `ExitTimeSeconds` and `CanInterrupt`. **At runtime it plays only the entry state** (or the first blend-tree child): see `ResolveControllerPlayback` in `OE/Engine/Source/Engine/Rendering/AnimatorSourceResolution.cpp:145-168`. Transitions, exit time and interruption are parsed but unused; the only runtime FSM is the graph path.
- Other pieces: `TweenService` (delay, loop count, yoyo, easing, callbacks), `SpriteFrames` (Godot-style named sprite animations), `AnimationTimeline` (Godot-style multi-track: property, audio, method, event and bezier tracks), `AnimationLibrary`. GPU animation sampling (`GPUAnimationDataStore`, with a fallback in `AnimationSystem.cpp:966`) is in the render domain.
- **No clip compression or quantization** (searched for compress/quantiz/acl/ozz).
- Editor: `OE/Apps/Editor/Source/Panels/AnimationWindowPanel.cpp` (12.9k LOC, dope sheet), `CurvesGraphView.cpp` (3.8k LOC, curve editor with tangent handles and extrapolation display), `AnimationGraphPanel.cpp` (node graph on their `Graph` module), `HumanoidRigInspector.cpp`, `RetargetMapInspector.cpp`, `AnimatorInspector.cpp`, `AnimationPreviewManager.cpp`. All of it is built on their own retained UI (uxml/css), not ImGui.

### 1.2 Physics (`OE/Engine/Modules/Physics` 3.2k LOC, `PhysicsECS` 3.5k LOC) — **Jolt only, 3D only**
- `IPhysicsBackend.h` abstracts the backend, with a single implementation in `Backend/JoltPhysicsBackend.cpp` (1880 LOC). Handles are generational.
- Shapes (`PhysicsShapes.h`): sphere, box, capsule (X/Y/Z axis), plane, convex hull, mesh, compound, heightfield.
- **Joints/constraints: none.** `PhysicsConstraints.h` declares a `DistanceConstraintSettings` that nothing uses.
- **Character controller**: `CharacterSettings` with a Kinematic motor (`JPH::CharacterVirtual::ExtendedUpdate` with stair step-up and stick-to-floor, `JoltPhysicsBackend.cpp:578-622`) and a Dynamic motor (`JPH::Character`). The ECS side is `PhysicsECS/Components/CharacterController.h` plus `Systems/CharacterControllerSystem.cpp`: desired velocity, jumping through `desiredVelocityY`, root-motion displacement, grounded state and ground normal, and it refuses to coexist with `PhysicsBody`. One tuning detail matters: `mWalkStairsMinStepForward = max(0.02, radius)`, because Jolt's defaults fail on typical capsules.
- Queries (`PhysicsQuery.h`): RayCast, RayCastAll and BoxOverlap, each with a `CollisionFilter` (layer mask, ignore sensors). **No shape casts and no sphere or capsule overlaps.**
- Filtering: a 32-layer `LayerCollisionMatrix` plus `CollisionGroup` and two broadphase layers (static/dynamic).
- Events: contact begin/persist/end with point, normal and impulse, and trigger enter/exit, buffered and dispatched (`PhysicsEventsSystem.cpp`, `PhysicsEntityEventCallbacks.cpp`). Script subscriptions go through `PhysicsScriptApi.h`.
- **In-place heightfield region update** (`UpdateHeightFieldRegion`, `JoltPhysicsBackend.cpp:897+`) uses `HeightFieldShape::SetHeights` with block alignment, an encode-range check and a broadphase refresh, plus `ActivateBodiesInAABB` to wake sleepers. Readiness gating lives in `PhysicsECS/HeightFieldCollisionReadiness.cpp`.
- Forces and impulses can be applied at a position (used for buoyancy in their ocean module). There is a fixed-timestep accumulator with max substeps.
- **No ragdolls, vehicles, soft bodies or debug draw.** The editor has a raycast debug-server handler (`OE/Apps/Editor/Source/DebugServer/PhysicsRaycastRequest.cpp`), with collider editing in `PhysicsColliderInspector.cpp`.

### 1.3 Audio (`OE/Engine/Modules/Audio` 2.8k LOC, `OE/Engine/ECSModules/Audio`) — miniaudio
- `AudioSystem.h` provides `Play2D` and `Play3D`, live volume/pitch/pan/pause/resume control, `SetFalloff` (None/Inverse/Linear/Exponential, with documented sanitizing in `AudioTypes.h`), up to 4 listeners and 256 voices.
- **Fixed bus set**: Master, Music, SFX, UI, VO, Aux, as `ma_sound_group` (`Source/MiniaudioBackend.h:75-80`). **Per-bus VU metering** comes from a custom `ma_node` that writes peaks to atomics (`BusMeterNode`).
- **Stream vs decode policy**: `OE/Engine/Source/Assets/AudioAsset.cpp:127-160`. WAV always decodes to PCM; files of 256 KB or less decode; clips of 3 s or less decode; clips over 10 s stream. It can be overridden per asset.
- Voice stealing takes the oldest voice, preferring pending ones (`AudioSystem.cpp:1010`). There is a "Silent" null-backend output for CI or headless runs, a master mute that leaves the mix running, an output PCM capture callback (for recording) and a latency query. `SpatializerHandedness.h` documents miniaudio's left/right handedness pitfall.
- **No effects, reverb, filters, HRTF, ducking or snapshots.** Formats: wav, mp3, flac and ogg (through libvorbis).

### 1.4 Input (`OE/Engine/Modules/Input` 4.9k LOC, 2.1k of which are tests) — GLFW
- `InputSystem.h` has raw polling with **frame edges** (`WasKeyPressed/Released`, `WasMouseButtonPressed/Released`, catching press+release within one frame), mouse delta (raw motion where available), pointer lock with an Unlocked/Pending/Locked state granted by the host, pointer-in-window and focus state, and per-button press origin.
- **Action maps**: `ActionDesc` holds bindings across keyboard, mouse button, mouse axis, gamepad button and gamepad axis, with scale and required/forbidden modifiers (`InputScriptApi.h:249`). Actions live in **contexts** with a push/pop priority stack and enable flags, and the API offers `GetActionState`, `WasActionTriggered`, listeners with `consumed` propagation, and host-reserved actions.
- **Sink chain** (`SinkRole::Gameplay/Application`, `IRawInputSink`): a gameplay sink claims only the keys it binds or polls, and the rest go to the editor. This is the cleanest part of the module.
- **Binding persistence is not implemented**: `InputSystem.cpp:698-706` returns false. There is no rebinding UI, **no touch**, no deadzones, no rumble, no gamepad DB update (raw GLFW joystick fallback in `GamepadPolling.cpp`), and the editor's shortcut actions are separate (`OE/Apps/Editor/Source/EditorInputActions.cpp`).

### 1.5 Text (`OE/Engine/Modules/Text` 3.4k LOC) — FreeType + HarfBuzz + **Slug**
- `FontAtlas.h` renders glyphs as **GPU quadratic-Bézier coverage (Slug algorithm)**. `FontAtlas_Slug.cpp` decomposes FreeType outlines (cubics are split into quadratics) and builds curve and band textures (RGBA16F curves, RG16UI bands, fixed-size append-only 4096-wide pages). The shader `OE/Engine/Modules/Rendering/Shaders/UI/slug_functions.glsl` says it is "ported from Eric Lengyel's reference HLSL (MIT)" and clamps loop bounds against hostile texel data. Text stays sharp at any scale.
- Color emoji go to RGBA atlas pages. HarfBuzz shaping, CSS letter-spacing semantics, Blink-matched line metrics, caret maps (`BuildCaretMapUtf8`) and a shaped-width cache are included (`FontAtlas_Measure.cpp`, `TextLayout.cpp`).
- **No real bidi**: `FontAtlas_Internal.h:80-110` picks the direction with a first-strong heuristic over Hebrew/Arabic code-point ranges. There is no UAX#9 and no multi-font fallback chain (one face per atlas).

### 1.6 Mesh picking (`OE/Engine/Modules/MeshPicking`, 1.75k LOC with 387 LOC of tests)
- `MeshBvh.h/.cpp` builds a binned-SAH BVH, collapses it to a **CWBVH-style 8-wide BVH with 8-bit quantized child AABBs**, and traverses it with **AVX2, NEON or scalar** code (`MeshBvh.cpp:22-33, 541-771`). Möller-Trumbore intersection returns barycentrics. It has a serialize/deserialize disk cache and a brute-force path below 256 triangles.
- Dependencies are only `Mathematics` (Vector3, AABB) and `Types`.
- Consumers are in the editor: `OE/Apps/Editor/Source/Picking/MeshPickingService.cpp`, `MeshBvhCache.cpp`, `MeshBvhDiskFormat.cpp`, `SceneView/SceneViewDropPlacement.cpp` (drop onto a surface) and `Placement/SplineSurfaceConform.cpp`.

---

## 2. What OURS has

### 2.1 Animation (`engine/core/action`, `engine/core/component`, `engine/core/subsystem/ActionSystem.cpp` 2020 LOC)
- An entity- and action-based model (from Doriax). `Animation` (`engine/core/action/Animation.h`) is an Action holding `ActionFrame`s (start time, duration, target, track). Keyframe tracks are separate action entities: `action/keyframe/{Translate,Rotate,Scale,Morph}Tracks`, with `KeyframeTracksComponent` (times plus per-segment `EaseType`) and Hermite in/out tangents for glTF CUBICSPLINE (`RotateTracksComponent.h`).
- Bones are **entities** (`object/Bone.h`, `BoneComponent.h` with bind TRS and inverse bind matrix). Poses therefore live in bone Transforms, not in pose arrays. The skinning palette holds up to `MAX_BONES` = 128 (`engine/core/Engine.h:107`).
- **Crossfade**: `AnimationComponent` has weight, fadeTarget, fadeSpeed and stopOnFadeOut. `Model::playAnimation(name, fadeTime)` (`object/Model.h:35-43`) and `ActionSystem` accumulate weighted TRS per target (`ActionSystem.cpp:1378-1430`, nlerp-style quaternion average, `flushPoseBlend`). This is effectively N-way weighted blending, without masks or additive layers.
- Tweens: `Tween` and the Position/Rotation/Scale/Color/Alpha tweens, with `Ease.h` (460 LOC; quad through elastic, back, bounce, step and custom), each with a loop flag. Sprite animation is `SpriteAnimationComponent` (frames plus per-frame times). Morph targets are animated.
- **Missing**: state machine, blend trees, IK, retargeting, root motion, animation events, masks/layers/additive, montages and compression.
- Editor: `editor/window/AnimationWindow.cpp` (3.1k LOC) is a track/action-frame timeline with keying, preview, crossfade/transition preview and snapping. **There is no curve editor**: tangents are only kept mirrored when keys are edited (`AnimationWindow.cpp:543-600`).

### 2.2 Physics (`engine/core/subsystem/PhysicsSystem.cpp` 3227 LOC) — **Box2D v3 (2D) + Jolt (3D)**
- 2D: rigidbodies and colliders, plus **distance/rope, revolute, prismatic, mouse, wheel, weld and motor joints**. Events cover begin/end contact, sensor begin/end, hit, preSolve and shouldCollide (`PhysicsSystem.h`).
- 3D colliders are sphere, box, capsule, **tapered capsule, cylinder**, convex hull, mesh and heightfield, built from raw data or entity meshes or heightfields (`RigidbodyComponent.h:21-38`). Motion quality covers discrete and LinearCast CCD, and allowed DOFs can be set.
- **3D joints**: fixed, distance, point, hinge, cone, prismatic, swing-twist, six-DOF, path, gear, rack-and-pinion and pulley. This goes far beyond theirs, which has no joints at all.
- Events: body activated/deactivated, contact added/persisted/removed and shouldCollide. Unity-style layer collision matrices exist for both 2D and 3D, with `Physics::raycast/raycastAll` and `Physics2D::raycast/raycastAll` taking layer masks (`engine/core/registry/PhysicsLayers.h`). `Ray::intersects` has many overloads with category and mask bits (`engine/core/math/Ray.h`).
- The editor draws collider wireframes in 2D and 3D (`editor/render/SceneRender2D.cpp:307+`, `SceneRender3D.cpp:665+`). The runtime `PhysicsSystem::draw()` is empty.
- **Missing**: character controller, overlap and shape-cast queries, in-place heightfield updates (the heightfield is rebuilt from `HeightFieldShapeSettings`, `PhysicsSystem.cpp:1371`), ragdolls and vehicles. Our Jolt copy in `engine/libs/joltphysics` includes all of these.

### 2.3 Audio (`engine/core/subsystem/AudioSystem.cpp` 443 LOC) — SoLoud
- `AudioSourceComponent`: volume, speed, pan, looping with a **loop point**, protect voice, inaudible behavior, min/max distance, attenuation models, **doppler factor**, triggers and callbacks. There is one 3D listener (the camera). Global volume and pause/resume/stopAll are available, and the system degrades gracefully when no output device exists (`outputUnavailable`).
- **Only `SoLoud::Wav`** is used (full decode). There are no buses, filters or streaming, even though `engine/libs/soloud` already ships `Bus`, `WavStream` and filters (`src/filter/`: freeverb, echo, biquad, EQ, lofi, flanger, duck, and others).

### 2.4 Input (`engine/core/Input.h`, `engine/core/Engine.h:546-592`, `engine/platform/*`)
- Static polling: `isKeyPressed`, `isMousePressed`, mouse position and scroll, **multi-touch** (`getTouches`), and gamepads (connected, name, buttons, axes).
- Event subscriptions: `onKeyDown/Up`, `onTouchStart/Move/End/Cancel`, `onGamepadConnect/Disconnect/ButtonDown/Up/AxisMove`, with HTML UI consumption.
- `System::setMouseMode` (NORMAL/HIDDEN/CAPTURED/CONFINED, `engine/core/System.h:55`), plus pointer lock on the web (`engine/platform/emscripten/JasperEngineWeb.cpp:491`).
- `generate_gamepad_mappings.py` embeds SDL_GameControllerDB for `glfwUpdateGamepadMappings` (THEIRS has nothing like it). `editor/window/PlayInput.cpp` forwards Game-view input and releases stuck keys, with support for MCP pointer injection.
- **Missing**: action maps and contexts, rebinding, frame-edge polling (`wasPressed`; only events or level state exist), deadzones and rumble.

### 2.5 Text (`engine/core/util/{FontFace,TextShaper,STBText,FontRegistry}.cpp`, `engine/core/htmlui` with RmlUi)
- stb_truetype coverage bitmaps in a per-size atlas (`FontFace::rasterizeGlyph`), with **HarfBuzz shaping and SheenBidi UAX#9 bidi** (`TextShaper.cpp:332-364`).
- A **font fallback chain** that picks faces per grapheme cluster the way Chromium does, with UAX#29 cluster logic, default-ignorable handling, letter and word spacing, and justification (`TextShaper.h`). Generic CSS family mapping is in `FontRegistry.cpp`. The full HTML/CSS UI goes through RmlUi.
- **Missing**: resolution-independent glyphs (SDF, MSDF or curve rendering) and color emoji (COLR/CBDT).

### 2.6 Picking
- Editor picking tests rays against **world AABBs only**, with terrain as a special case (`editor/Project.cpp:6134-6178` `findBestEntityByRay`). There is no triangle-accurate mesh raycast anywhere in the engine (`Ray.h` covers planes, AABBs, OBBs, spheres and rigidbodies).

---

## 3. Gaps — THEIRS has, OURS lacks

| Feature | Their location | Value to us | Effort |
|---|---|---|---|
| Triangle-accurate mesh raycast (BVH8, SIMD) for editor picking, surface drop, runtime `Ray` vs mesh | `OE/Engine/Modules/MeshPicking/*` | **H** | **S–M** |
| Animation events: time-based, loop- and reverse-safe, script-pollable | `OE/Engine/Modules/Animation/{Include,Source}/AnimationEvent.*` | **H** | **S–M** |
| Event, loop and speed declarations in glTF `extras.clip` | `OE/docs/animation/gltf-clip-schema.mdx`, parsing in `OE/Engine/Source/Assets/AnimationClip.cpp` | M | S |
| 3D character controller (CharacterVirtual: stairs, slopes, grounded) | `OE/Engine/Modules/Physics/Source/Backend/JoltPhysicsBackend.cpp:578-622, ~1700-1800`; `OE/Engine/Modules/PhysicsECS/Source/Systems/CharacterControllerSystem.cpp` | **H** | M |
| 1D/2D blend spaces (locomotion) | `OE/Engine/Modules/Animation/Source/Nodes/BlendSpace{1D,2D}Node.cpp` | **H** | M (the weight maths is S; the authoring UI is M) |
| Animator state machine (params, conditions, crossfade) | `OE/Engine/Modules/Animation/Source/Nodes/StateMachineNode.cpp`, `AnimationController.*` | H | L (needs an asset, runtime and editor; their FSM is minimal) |
| Two-bone IK, FABRIK, look-at | `OE/Engine/Modules/Animation/Source/Nodes/{TwoBoneIK,FABRIK,LookAt}Node.cpp`, `Source/PoseUtilities.cpp` | M–H | M |
| Foot IK and foot lock | `OE/Engine/Modules/Animation/Source/Nodes/FootIKNode.cpp`, `Source/Ops/FootLockOp.cpp` | M | M (needs IK and a controller first) |
| Root motion feeding the character controller | `OE/Engine/Source/Engine/Rendering/Systems/AnimationGraphSystem.cpp:76-96, 380-388`; `OE/Engine/Modules/Animation/Source/RootMotionExtractor.cpp` | M | S, once a controller exists |
| Bone masks, layered and additive blending | `OE/Engine/Modules/Animation/Source/BoneMask.cpp`, `Source/Nodes/{LayeredBlend,AdditiveBlend}Node.cpp` | M | M |
| Montages (sections, blend in/out, slot) | `OE/Engine/Modules/Animation/Source/{AnimationMontage,MontageInstance}.cpp`, `Source/Nodes/MontageSlotNode.cpp` | M | M |
| Humanoid retargeting (auto-map, VRM/KHR) | `OE/Engine/Modules/Animation/Source/{HumanoidRig,HumanoidNameMatcher,HumanoidImport,RetargetMap,SkeletonProfile}.cpp`, `Source/Nodes/RetargetNode.cpp` | M | **XL** |
| Curve editor (tangent modes, extrapolation) | `OE/Apps/Editor/Source/Panels/CurvesGraphView.cpp`; `OE/Engine/Modules/Animation/Include/Animation/AnimationClip.h` enums | M | L (rewrite in ImGui) |
| Input action maps and contexts with consumption | `OE/Engine/Modules/Input/Include/Input/InputSystem.h`, `InputScriptApi.h` | **H** | M |
| Frame-edge input queries (`wasPressed/Released`, including press+release in one frame) | `OE/Engine/Modules/Input/Source/InputSystem.cpp` | **H** | **S** |
| Mouse delta from raw motion | `OE/Engine/Modules/Input/Source/InputSystem.cpp` | M | S |
| Box overlap query with filter | `OE/Engine/Modules/Physics/Source/Backend/JoltPhysicsBackend.cpp:1494-1540` | M | S |
| In-place heightfield collider update while sculpting | `OE/Engine/Modules/Physics/Source/Backend/JoltPhysicsBackend.cpp:897-1000`, `OE/Engine/Modules/PhysicsECS/Source/HeightFieldCollisionReadiness.cpp` | M | S–M |
| Audio buses with volume and VU meters | `OE/Engine/Modules/Audio/Source/MiniaudioBackend.{h,cpp}` | M | S (use SoLoud::Bus) |
| Stream vs decode policy for long clips | `OE/Engine/Source/Assets/AudioAsset.cpp:127-160` | M | S (SoLoud::WavStream) |
| Headless "silent" output, output capture | `OE/Engine/Modules/Audio/Include/Audio/AudioTypes.h`, `AudioSystem.h` | L–M (CI, video recording) | S |
| Resolution-independent text (Slug curves), color emoji pages | `OE/Engine/Modules/Text/Source/FontAtlas_Slug.cpp`, `OE/Engine/Modules/Rendering/Shaders/UI/slug_functions.glsl` | M | L |
| Tween service extras (delay, loop count, yoyo) | `OE/Engine/Modules/Animation/Source/TweenService.cpp` | L | S |

**Neither engine has**: ragdolls, vehicles, physics debug draw at runtime, audio effects or reverb (we have SoLoud filters unused), clip compression, rebinding UI or persistence, MSDF text, or IK in the modifier stack (their stubs do nothing).

---

## 4. Who does what better

### THEIRS does better
- **Picking**: a triangle BVH with barycentrics against our AABB-only selection (`editor/Project.cpp:6168`). Clicking a large or concave mesh in our editor picks by bounding box, which can select the wrong entity.
- **Animation architecture**: pose arrays flow through a node graph with a per-frame `EvaluationContext`. That makes IK, masks, additive layers and blend spaces composable. Our entity-per-bone model with weighted accumulation per target handles crossfades but cannot express "upper body plays X, legs play Y" or post-pose IK without a new pass.
- **Event semantics** are rigorous: wrap, whole-lap and backward steps, with dedupe across skinned meshes sharing one animator (`AnimationEvent.cpp`), backed by tests (`Tests/AnimationCoreTests.cpp`, 886 LOC).
- **Character controller wiring**: a two-motor design, a stair-tuning fix, root-motion displacement converted to velocity, and refusal of conflicting components.
- **Input semantics**: sink claiming and consumption between the game and the editor, a host-granted pointer-lock state, and edge polling that never loses a tap within one frame. Our `PlayInput.cpp` also deals with game/editor routing, but the game side has only level and event APIs.
- **Audio robustness**: a null backend, a mute that keeps the mix running, metering, voice stealing, and documented falloff sanitizing. The engineering and test discipline in their modules is generally higher (Input has 2.1k LOC of tests, Animation about 7k).
- **Text rendering quality at scale**: Slug curves have no atlas blur when zoomed and handle 3D or world-space text cleanly.

### OURS does better
- **Physics breadth**: full 2D physics (Box2D v3) with 7 joint types, 12 3D joint types, more collider shapes (tapered capsule, cylinder), CCD, DOF locks and preSolve/shouldCollide hooks. THEIRS has **no joints and no 2D**.
- **Text correctness**: real UAX#9 bidi (SheenBidi), a multi-face fallback chain with per-grapheme-cluster selection, word spacing and justification. Their RTL handling is a code-point-range heuristic with one face per atlas.
- **Input platform coverage**: touch, an SDL gamepad DB (`generate_gamepad_mappings.py`), web pointer lock, and four mouse modes. THEIRS has no touch, no gamepad DB, and binding persistence is a stub.
- **Audio features per source**: doppler, loop point, voice protection and inaudible behavior. SoLoud also already has buses, streaming and filters (freeverb, EQ and others) that miniaudio does not offer as ready-made effects in their setup. Our gaps here are wiring, not missing libraries.
- **Editor collider visualization** in 2D and 3D. Their editor has only inspectors and a raycast debug handler.
- **Honest scope**: some of their features exist only on paper. The AnimationController ignores transitions at runtime (`AnimatorSourceResolution.cpp:157-167`), the SkeletonModifierStack implements only CopyTransform, FootIK is not compiled into graphs, root motion always uses bone 0, and the clip `translations`/`rootMotion` fields are stored but unread. Counting their features by enum or header overstates them.

---

## 5. Copy/port candidates (ranked by value/effort)

License: OpenEngine is MIT (`OE/LICENSE`, "Copyright (c) 2026 Gamecrafters Guild Limited and contributors"). A port must keep that copyright notice, for example a header line plus an entry in `NOTICE.md`. None of the candidates below uses code from `OE/ThirdParty` (whose folders carry their own licenses).

1. **MeshBvh (triangle picking)**: copy and adapt.
   - Source: `OE/Engine/Modules/MeshPicking/Include/MeshPicking/{MeshBvh.h,PickTypes.h}`, `Source/MeshBvh.cpp`, plus the tests in `Tests/MeshBvhTests.cpp` as a reference.
   - Dependencies: `Mathematics::Vector3` and `AABB`, and `Types` (uint32/float32). Replace them with our `Vector3`/`AABB`. The `-mavx2 -mfma` CMake check is in `OE/Engine/Modules/MeshPicking/CMakeLists.txt:13-16`. On wasm the scalar path is used; WASM SIMD can come later.
   - Integration: build a BVH per mesh submesh (cache it keyed by buffer), skinned meshes fall back to the AABB, and `findBestEntityByRay` refines AABB hits with the triangle hit. Then expose `Ray::intersects(Mesh)` to Lua. Their `OE/Apps/Editor/Source/Picking/MeshPickingService.cpp` and `MeshBvhCache.cpp` show the cache and invalidation pattern.
   - Difficulty: **S–M**. This is the cleanest, self-contained module in scope.

2. **Animation event track and crossing logic**: copy.
   - Source: `OE/Engine/Modules/Animation/{Include,Source}/AnimationEvent.*` (about 250 LOC, std only).
   - Adaptation: add an `AnimationEventTrack` to `AnimationComponent`. In `ActionSystem` compute a `PlaybackStep` from the previous and current timecount (loop gives Wrapped or WholeLap, negative speed gives Backward), then fire `FunctionSubscribe<void(std::string, payload)>` and Lua callbacks. Fill events from glTF `extras.clip.events` in our tinygltf importer, following `OE/docs/animation/gltf-clip-schema.mdx`, which is a good spec to reuse as-is. Add an event lane to `AnimationWindow`.
   - Difficulty: **S–M**.

3. **Jolt CharacterVirtual controller**: adapt.
   - Source: `OE/Engine/Modules/Physics/Source/Backend/JoltPhysicsBackend.cpp`, the kinematic and dynamic character create path (`CreateCharacter`, about lines 1700-1800) and the step loop (578-622). Also `OE/Engine/Modules/PhysicsECS/Include/PhysicsECS/Components/CharacterController.h` and `Source/Systems/CharacterControllerSystem.cpp`.
   - Dependencies: Jolt, which we already have. Their generational handles and ECS query API would be replaced by our ECS and `PhysicsSystem`.
   - Add a Unity-like `CharacterControllerComponent` (radius, height, slope, step offset, skin width, `move(velocity)`, `isGrounded`, ground normal) and set up layer filters with our `JoltObjectLayerTable`. Also add a `CharacterContactListener` for pushing bodies; theirs lacks one.
   - Difficulty: **M**.

4. **Blend-space weight solvers, used with our existing weighted accumulation**: adapt.
   - Source: `OE/Engine/Modules/Animation/Source/Nodes/BlendSpace1DNode.cpp` (99 LOC) and `BlendSpace2DNode.cpp` (183 LOC). Only the weight computation is needed; the pose evaluation is theirs.
   - Our `ActionSystem` already blends any number of Animations on the same bones by `AnimationComponent.weight`. So a `BlendSpaceComponent` (params, samples mapped to Animation entities) only has to compute weights each frame, keep the sample clips phase-synced (normalized time), and set each `Animation` weight. Consider replacing their inverse-distance-weighted 2D blend with a gradient-band or barycentric version for better locomotion.
   - Dependencies: none.
   - Difficulty: **M**, including inspector UI and phase sync.

5. **Frame-edge polling plus a minimal action map**: re-implement, using theirs as the design reference.
   - Reference: `OE/Engine/Modules/Input/Include/Input/InputSystem.h` (the `ActionDesc`/`ActionBinding` model with scale and required/forbidden modifiers, contexts with push/pop priority, `GetActionState`/`WasActionTriggered`, edge semantics) and `InputScriptApi.h:249-388` (script surface).
   - Do not copy `InputSystem.cpp` wholesale. It is entangled with their sink, claim and pointer-lock routing, and its JSON persistence is a stub.
   - For us: add per-frame `justPressed`/`justReleased` sets to `engine/core/Input.cpp`, cleared in `Engine` after update (S). Then add an `InputActions` project asset (YAML via yaml-cpp) with keyboard, mouse, gamepad and touch bindings, deadzones and rebinding persistence. Touch and deadzones would go beyond what theirs offers.
   - Difficulty: S for edges, M for action maps.

Next candidates, ranked lower:
- 6. **Two-bone IK, FABRIK and look-at solvers** (`OE/Engine/Modules/Animation/Source/Nodes/{TwoBoneIKNode,FABRIKNode,LookAtNode}.cpp`, `Source/PoseUtilities.cpp`, `Include/Animation/QuaternionMath.h`). The maths is portable. The solvers need a model-space pose gathered from our bone-entity Transforms and written back after `flushPoseBlend`, as a post-animation `IKComponent` pass. Their static `SolveTwoBoneIK` ignores the pole target (only the member path uses it), so fix that when porting. M.
- 7. **Heightfield in-place update** (`OE/Engine/Modules/Physics/Source/Backend/JoltPhysicsBackend.cpp:897-1000` plus `ActivateBodiesInAABB`). This would make terrain sculpting in Play mode cheap, using Jolt `SetHeights` with block alignment and an encode-range fallback to a rebuild. S–M.
- 8. **Audio stream/decode heuristic and bus layout**. Port the policy from `OE/Engine/Source/Assets/AudioAsset.cpp:127-160` (use `SoLoud::WavStream` when it says stream). Build Master, Music, SFX, UI, VO and Aux as `SoLoud::Bus` instances with an `AudioSourceComponent.bus`. SoLoud's `Bus::calcActiveVoices` and `getApproximateVolume` provide VU readings, so the miniaudio node does not need porting. S.
- 9. **Root-motion-into-controller wiring** (`OE/Engine/Source/Engine/Rendering/Systems/AnimationGraphSystem.cpp:76-96`), once item 3 exists. Make the bone configurable, unlike theirs. S.
- 10. **Slug text** (`OE/Engine/Modules/Text/Source/FontAtlas_Slug.cpp`, `SlugGlyphData.h`, `OE/Engine/Modules/Rendering/Shaders/UI/slug_functions.glsl`).
  - Adaptation: their builder uses `FT_Outline_Decompose`. We use stb_truetype, so rebuild the curve list from `stbtt_GetGlyphShape` (TrueType is quadratic; split CFF cubics the way their `SlugCubicTo` does). The shader needs RGBA16F and RG16UI textures with integer `texelFetch`, which sokol and WebGL2 support, plus bindless arrays that we would replace with per-page bindings.
  - Licensing: the shader comments credit Lengyel's MIT reference. **Before shipping, check the status of Lengyel's Slug patent** (US 10,373,352 family), and that MIT covers the reference code.
  - Difficulty: L.

---

## 6. Not worth copying

- **AnimationController asset and runtime**: transitions, exit time and interruption are parsed but never evaluated (`OE/Engine/Source/Engine/Rendering/AnimatorSourceResolution.cpp:145-168`). Design our own Unity-style Animator; `AnimationController.h` is useful only as a JSON field checklist.
- **SkeletonModifierStack**: 8 of its 9 modifier kinds are empty stubs (`SkeletonModifierStack.cpp:64-75`).
- **Humanoid retargeting pipeline**: XL, deeply tied to their `SkeletonData`/`SkeletonProfile`/asset watcher and sidecar files, with cross-profile axis frames still unfinished (`RetargetNode.h`). If we need retargeting, `HumanoidNameMatcher.cpp` (bone-name auto-mapping heuristics) is the only piece worth reading.
- **Editor animation panels** (`AnimationWindowPanel.cpp` 12.9k LOC, `CurvesGraphView.cpp`, `AnimationGraphPanel.cpp`): written against their retained uxml/css UI and `Graph` module, so they cannot be ported to our ImGui editor. Copy the UX ideas (tangent-type menu, extrapolation modes, pinned reference curves) instead.
- **Physics backend abstraction** (`IPhysicsBackend`, generational handles): we use Jolt and Box2D directly with richer features. Their abstraction would add a layer and remove joints.
- **miniaudio backend**: switching from SoLoud loses doppler, filters and loop points, and SoLoud already covers buses and streaming.
- **The InputSystem implementation as a whole**: its sink/claim routing fits their editor architecture (`WindowInputRouter`), it has no touch, and its persistence is a stub. Take the data model only (candidate 5).
- **Their text shaping and bidi**: worse than our SheenBidi plus fallback-chain `TextShaper`. Only the Slug renderer (candidate 10) and possibly the caret-map API are of interest.
- **GPU animation sampling** (`GPUAnimationDataStore`): it needs compute, which our GLES3/WebGL2 targets lack, and it belongs to the render domain.
- **FootIKNode as it stands**: it is not wired into their graph compiler and needs raycasts from the caller. Port TwoBoneIK first and write foot IK on top of our own raycasts.
