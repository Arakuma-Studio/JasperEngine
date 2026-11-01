# Animation layers, avatar masks and Animator Controllers (`anim-layers`, ledger #10)

Status: specced 2026-10-31. Branch `gaps/anim-layers`. Two phases on one branch. Phase 1 is
**layers + masks** in the Animation Player. Phase 2 is the **Animator Controller**: a data-driven
state machine asset with an editor window. Phase 2 starts only when phase 1 is complete, tested
and reviewed. Phase 1 can be merged alone if the lead decides so.

Read first: [AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) (the shared conventions are binding),
[docs/animation-clips.md](../../animation-clips.md), and
[2026-10-24-animation-clips-design.md](2026-10-24-animation-clips-design.md). That spec's rules R1
(blending), R1b (held states), R2 (rest values), R9 (rebinding), R16 (events) and R17 (queue) stay in
force unless a rule below changes them.

## 1. Goals

- Unity-style **layers** in the Animation Player. Each layer has a name, a weight (with fades), a
  blend mode (Override or Additive) and an optional **mask**. Each clip entry sits on one layer.
- `play` / `crossFade` / `queue` work **per layer**: Unity's `PlayMode.StopSameLayer` by default,
  and `StopAll` on request.
- **Avatar Mask** assets (`.mask`) with two parts:
  - humanoid body parts, using the engine's existing humanoid map;
  - transform paths, each with a weight and a children flag.
- Legacy per-state **mixing transforms** (`AnimationState.AddMixingTransform`).
- Clip events, `onFinish`, the R2 rest rule and the edit-mode preview all keep working with layers.
- **Phase 2:** an **Animator Controller** asset (`.controller`):
  - parameters (float, int, bool, trigger);
  - per layer, a state machine with an Any State, transitions with conditions, exit time, duration,
    offset and interruption;
  - 1D blend trees.
  - It is played by the same Animation Player and edited in a new **Animator** window and by MCP.
  - Lua gets `setFloat`, `setBool`, `setTrigger` and the rest.
- The case to solve (GAP-REPORT §5): the Garden farmer.
  - Its base layer goes between Idle and Running on a float `Speed` (> 0.01 and < 0.01, 0.15 s fixed
    fades).
  - Its upper layer "Holding" plays a right-arm pose through a right-arm mask. A script lerps the
    layer's weight towards 0 or 1 at `dt*15` (`GPlayerHoldItem.cs:36`).
  - It works with layers alone (Lua drives the base) and with a controller.
- Existing scenes, prefabs and `.anim` files play byte-identically. A player with one layer and no
  mask uses today's code path.

## 2. Non-goals

These are out for now. Each one is listed as a follow-up.

- Animator features we leave out:
  - synced layers (`m_SyncedLayerIndex`);
  - IK pass and IK goals in masks;
  - sub-state machines, Exit nodes, state machine behaviours with C# lifecycle (we offer only
    `onStateEnter` / `onStateExit` callbacks);
  - 2D blend trees and direct blend trees;
  - Animator Override Controllers;
  - root motion;
  - mirror, foot IK and humanoid playback, which belong to `anim-retarget`: phase 2 only parses and
    passes those state flags through.
- Importing Unity `.controller` and `.mask` YAML. These are refused with a message, the same way
  `.anim` YAML is refused today.
- Blending on layered players in mesh space (Unreal's "mesh space rotation blend"). Blending is in
  local space, as Unity does it.
- Animating layer weights from clips. Use Lua instead.

## 3. Users and stories

- **Porter (human, from Unity).**
  - "My farmer walks. While he holds a watering can, his right arm holds it." The porter adds a layer
    "Holding" with an Avatar Mask that ticks only the right arm and fingers, and puts the `Holding`
    clip on it.
  - In Lua: `player:setLayerWeight("Holding", w)`.
  - With a controller: the porter rebuilds GranjeroAnim in the Animator window (two layers, `Speed`
    float, transitions) and calls `player:setFloat("Speed", v)`.
- **Designer.** Creates an Avatar Mask by clicking body parts on a figure, or by ticking bones in a
  tree imported from the model. Scrubs the "Holding" clip in the Animation window over the walk with
  **Preview over: Walk**.
- **Gameplay programmer.**
  - Plays a hit reaction additively: an `additive` layer and `crossFade("Hit", 0.05)`.
  - Uses `queue` per layer.
  - Uses `PlayMode.STOP_ALL` to reset everything.
- **AI agent.**
  - Writes masks and controllers as JSON files, or through `edit_animator`, and reads clear warnings:
    a parameter with the wrong type, a dead-end state, an unreachable state.
  - Checks coverage with `get_animation {mask, root}`.
  - In play mode, reads live layer weights, parameters and current states (`get_animation {entity}`)
    and verifies bone poses numerically with `get_runtime_entity`.

## 4. Research summary

Sources:
- Unity legacy Animation: [AnimationState](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/AnimationState.html),
  [AddMixingTransform](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/AnimationState.AddMixingTransform.html),
  [CrossFade/PlayMode](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Animation.CrossFade.html),
  [Animation scripting: layers](https://docs.unity3d.com/2022.3/Documentation/Manual/AnimationScripting.html).
- Unity Animator: [layers](https://docs.unity3d.com/2022.3/Documentation/Manual/AnimationLayers.html),
  [AvatarMaskBodyPart](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/AvatarMaskBodyPart.html),
  [transitions](https://docs.unity3d.com/2022.3/Documentation/Manual/class-Transition.html),
  [AnimatorConditionMode](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Animations.AnimatorConditionMode.html),
  [1D blend trees](https://docs.unity3d.com/2022.3/Documentation/Manual/BlendTree-1DBlending.html).
- Godot: [AnimationTree](https://docs.godotengine.org/en/stable/tutorials/animation/animation_tree.html),
  [StateMachineTransition](https://docs.godotengine.org/en/stable/classes/class_animationnodestatemachinetransition.html),
  [BlendSpace1D](https://docs.godotengine.org/en/stable/classes/class_animationnodeblendspace1d.html),
  [AnimationNode filters](https://docs.godotengine.org/en/stable/classes/class_animationnode.html).
- Unreal: [Layered blend per bone](https://dev.epicgames.com/documentation/en-us/unreal-engine/animation-blueprint-blend-nodes-in-unreal-engine).

What we take, and what we leave:

- **We take Unity Animator layer semantics: weight, Override/Additive and mask.** We do not take the
  legacy rule where "higher layers consume weight first".
  - The game uses Animator layers.
  - An explicit layer weight is easier to reason about than weight left over from other layers.
  - For one full-weight state on the upper layer, the two rules give the same result.
- **We take legacy `PlayMode.StopSameLayer`, `AddMixingTransform` and per-layer `CrossFadeQueued`.**
  The Animation Player is the legacy Animation component, and Unity users of `Animation` expect them.
- **We take Unity's AvatarMask body part list.** Unity names, snake_case, without the four IK goals,
  because we have no IK goals. We also take its transform mask. The game's `Arm.mask` is
  `[0,0,0,0,0,0,1,0,1,0,0,0,0]`: RightArm and RightFingers only.
- **We take Unreal's fractional blend weight per bone, as a weight from 0 to 1 on mask entries.** We
  leave its blend depth: a weight per entry is simpler.
- **Additive reference.** Unity legacy adds the difference from the clip's first frame. We take that,
  with the `additive_reference` time the clip format reserved (clip spec 2.5).
- **Transitions: we take Unity's model (has exit time, exit time, fixed duration, offset, conditions,
  ordered Any State transitions, triggers reset when a transition consumes them).** We leave Godot's
  travel / AStar. Unity users know Unity's model, and it covers all 11 controllers of the game. The
  most complex one is RCC: two layers, 1D trees on `steer`, exit times.
- **Blend tree children: we take Godot's and Unity's synchronised normalized time.** For events we
  leave Unity's "fire on every child" in favour of "only the heaviest child", which stops double
  footsteps. This matches our crossfade rule: only the incoming clip fires.
- **Interruption: we support only `none`, `current_state` and `next_state` (ordered).** The full set
  of five Unity sources is rarely used. None of the 11 controllers in the game uses any.

## 5. Product decisions

1. **The Animation Player gets layers. There is no separate "Animator" component.** A controller is
   an optional field of the same component, and `GameObject:getAnimator()` is an alias of
   `getAnimationPlayer()`.
   - Why: one runtime, one Inspector section, one serialization and one MCP surface.
   - The glossary maps Unity's Animator to an Animation Player with a Controller.
2. **The layer list includes the Base Layer as index 0.**
   - The base layer's weight is always 1, and its blend mode is always Override.
   - It may have a mask. Masked-out bones of the base layer stay at their rest value.
   - An empty `layers` field means one Base Layer, so old files are unchanged.
3. **Clip entries get `layer` (an int, default 0).** Layers are addressed by index or by name in Lua.
4. **Composition order and maths** are in 7.3. Override layers are evaluated bottom to top with
   `lerp(previous, layerPose, layerWeight * mask * min(1, ΣstateWeights))`. Additive layers add the
   weighted delta from their reference frame.
5. **The v1 fast path.** A player whose layers are all defaults (one layer, no mask, no mixing
   transforms, no controller) keeps today's direct accumulation, so the existing tests pass
   unchanged. Every other player uses the layered evaluator. The evaluator then contributes its
   result to the shared pose blend with weight 1.
6. **Mask resolution happens at bind time, per bound track**, and is cached with the bindings. A mask
   reload or a rebind recomputes it. Sampling only multiplies by the cached weight.
7. **A layer whose mask is still loading contributes nothing.** A layer whose mask failed to load
   also contributes nothing, and logs its error once. Contributing nothing is safer than animating
   the whole body by mistake.
8. **Events.** A state's events fire under the clip spec's rule (weight > 0, not fading out) and, in
   addition, only when its layer's effective weight is above 0. The base layer always fires. Masks
   do not affect events.
9. **Layer weight fades are linear**, in the player's time (scaled, like state fades). The Unity
   pattern `Lerp(w, target, dt*15)` still works from Lua.
10. **Phase 2 controllers own the states.** With a controller, the player's `clips` and `layers` are
    ignored: the Inspector hides them and a warning names them.
    - Lua `play` and `crossFade` jump to controller states, using `"Layer.State"` or `"State"`.
    - Parameters persist on the player runtime.
    - Triggers stay set until a transition consumes them or `resetTrigger` is called.
11. **Controller validation is a first-class feature.** The editor, `check --anims` and MCP report:
    - unknown clips;
    - parameters that are missing or of the wrong type;
    - dead-end states (no outgoing transition, and the clip does not loop);
    - states no transition reaches;
    - transitions with neither conditions nor exit time;
    - parameters that no transition or blend tree reads ("unused").

    This catches the game's own bugs: `Watering` is never set and its state is a dead end, and
    `VictoryDance` is a trigger that the code sets as a bool.
12. **File formats are JSON, canonical and lenient**, like `.anim`: format plus version required, the
    rest dropped with a warning and a JSON path, unknown keys kept. `.anim` stays version 1:
    `additive_reference` is a reserved top-level key, and v1 readers already keep unknown keys.

## 6. File formats

### 6.1 `.anim` additions (version stays 1)

- `"additive_reference": <seconds>`: the time the clip's additive delta is measured from. The
  default is `0`, and it is written only when it is not 0. It is clamped to `[0, length]`.
  `ClipData::additiveReference` (float, default 0) and `AnimationClip::getAdditiveReference()` carry it.

### 6.2 Avatar Mask `.mask` (new)

```json
{
  "format": "jasperengine.mask",
  "version": 1,
  "guid": "3c1e...32 hex",
  "default": 0,
  "humanoid": {"root": 0, "body": 0, "head": 0, "left_leg": 0, "right_leg": 0,
               "left_arm": 0, "right_arm": 1, "left_fingers": 0, "right_fingers": 1},
  "transforms": [
    {"path": "Granjero/mixamorig_Hips/mixamorig_Spine/mixamorig_Spine1/mixamorig_Spine2/mixamorig_RightShoulder/Can", "weight": 1, "children": true}
  ],
  "preview": {"model": "models/garden/granjero.glb"}
}
```

| key | meaning |
|---|---|
| `default` | weight (0 to 1) of every entity that no rule below covers. Default **1**: an empty mask passes everything, as in Unity |
| `humanoid` | optional. Weight per body part, from 0 to 1 (`true` and `false` are read as 1 and 0). Parts not listed take `default`. Unity's IK part names (`left_foot_ik`...) are ignored with a warning |
| `transforms[]` | `path` (a clip path relative to the player root, same grammar as clip tracks), `weight` 0..1 (default 1), `children` (default true: the entry also covers descendants) |
| `preview.model` | editor only: the model or prefab the Inspector builds its tree and figure from |

Weight of an entity E under the player root R. The first rule that applies wins:
1. The transform entry whose path is E's own path. Otherwise, the entry for E's nearest ancestor that
   has `children: true`. The most specific (deepest) entry wins.
2. Otherwise, if `humanoid` is present and the root has a humanoid rig:
   - First the rig is found: the first ModelComponent on or under R in hierarchy order whose
     `buildModelRig` succeeds. This is the same rig the `anim-retarget` feature uses, and the same
     rule as `pose::PoseAnimatorSystem`.
   - Then E's part is found. A slot bone has its slot's part:
     - `hips`, `spine`, `chest`, `upper_chest` → body;
     - `neck`, `head` → head;
     - `<side>_shoulder`, `<side>_upper_arm`, `<side>_lower_arm`, `<side>_hand` → `<side>_arm`;
     - `<side>_upper_leg`, `<side>_lower_leg`, `<side>_foot`, `<side>_toes` → `<side>_leg`.
   - Any other entity below a slot bone (twist bones, fingers, props) takes the part of its nearest
     slot ancestor. Below a hand, that part is `<side>_fingers` instead.
   - `root` is R itself, plus every entity between R and the hips that is not a slot (the glTF root
     node, an armature).
3. Otherwise, `default`.

Masks name no scene ids. One mask works on every character with the same rig paths, and the humanoid
part works on any humanoid. Unity YAML (`%YAML` or `AvatarMask:`) is refused with "This is a Unity
.mask (YAML). Create a JasperEngine Avatar Mask (Assets > Create > Avatar Mask)."

### 6.3 Scene and prefab serialization (`editor/Stream.cpp` 4800, 5343, 7365)

```yaml
animationplayer:
  clips:
    - {clip: models/garden/granjero.glb#Happy Idle, name: Idle, wrap: loop}
    - {clip: models/garden/granjero.glb#Running, name: Running, wrap: loop}
    - {clip: anim/holding.anim, guid: ..., name: Holding, wrap: loop, layer: 1}
  layers:
    - {name: Base Layer}
    - {name: Holding, weight: 0, blend: override, mask: anim/arm.mask, maskGuid: ...}
  controller: anim/granjero.controller        # phase 2
  controllerGuid: ...
```

- `layer` is written only when it is not 0.
- `layers` is written only when it differs from the default. The default is a single Base Layer with
  no mask.
- Within a layer, `weight` is written only when it is not 1, and `blend` only when it is not
  override. `mask`/`maskGuid` and `controller`/`controllerGuid` follow the clip GUID rules: the GUID
  resolves first, the path is a hint, a rename repairs the path, and a deleted asset shows Missing.
- Prefab overrides work per field (`layers[1].weight`, `clips[2].layer`).
- The exported build path: `editor/Factory.cpp:1916` `createAnimationPlayerComponent` emits the new
  fields. `AnimationPlayerLayer` gets a Factory emitter the same way `AnimationPlayerClip` has one.
  `.mask` and `.controller` files ship automatically, because `Exporter::copyAssets` copies every
  asset.

## 7. Phase 1 design: runtime C++

### 7.1 Data (`engine/core/component/AnimationPlayerComponent.h`)

```cpp
enum class AnimationLayerBlend : uint8_t { Override, Additive };

struct JASPERENGINE_API AnimationPlayerLayer{
    std::string name;                 // unique in the player (case-sensitive); layers[0] "Base Layer"
    float weight = 1.0f;              // [0, 1]; layers[0]: ignored (always 1)
    AnimationLayerBlend blend = AnimationLayerBlend::Override;   // layers[0]: always Override
    std::string mask;                 // asset-relative .mask, "" = none
    std::string maskGuid;             // editor (AssetDatabase)
    bool operator==(const AnimationPlayerLayer&) const; bool operator!=(...) const;
};
// AnimationPlayerClip: + int layer = 0;   (operator== includes it)
// AnimationPlayerComponent: + std::vector<AnimationPlayerLayer> layers;   // empty = {Base Layer}
//                           + std::string controller, controllerGuid;   (phase 2)
//                           + FunctionSubscribe<void(int, const std::string&)> onStateEnter, onStateExit; (phase 2)
```

- `anim::playerLayers(const AnimationPlayerComponent&)` returns the effective list: `layers`, or one
  Base Layer when it is empty. Out-of-range entry layers clamp to 0, with a warning once.
- `anim::isDefaultLayering(player)` is true when: there is one layer, it has no mask, no state has
  mixing transforms, and there is no controller. When it is true, the v1 fast path is used.
- `AnimationPlayerComponent.h` contract header: append these invariants:
  - layers are authored fields;
  - runtime layer weights live in `AnimationPlayerRuntime::layers`.

### 7.2 Runtime state (`engine/core/animation/AnimationPlayerRuntime.h`)

```cpp
struct JASPERENGINE_API LayerRuntime{
    std::string name;
    float weight = 1, targetWeight = 1, fadeSpeed = 0;   // Lua setLayerWeight(…, fade)
    AnimationLayerBlend blend = AnimationLayerBlend::Override;
    std::string maskPath;                                 // runtime override (setLayerMask) or the authored one
    std::shared_ptr<const AvatarMask> mask;               // null = none; maskLoading / maskFailed below
    bool maskLoading = false, maskFailed = false; uint64_t maskGeneration = 0;
    std::deque<QueuedState> queue;                        // per-layer queue (R17 per layer)
};
struct MixingTransform{ std::string path; bool recursive = true; };
// PlayerStateRuntime: + int layer = 0 (the entry's; Lua state.layer overrides at runtime:
//   layerOverride = -1 none), + std::vector<MixingTransform> mixing,
//   + std::vector<float> trackMask (per clip track: layer mask * mixing weight, filled at bind),
//   + uint64_t maskStamp (the masks' generations it was computed for), + additive reference cache:
//   std::vector<anim::TrackSample> reference (sampled once per bind at additive_reference).
// AnimationPlayerRuntime: + std::vector<LayerRuntime> layers; `queue` moves into LayerRuntime.
```

- The runtime is rebuilt on `needsLoad`. This keeps each surviving layer's runtime weight by name,
  just as states keep time and weight (clip spec R14).
- Masks load through a new `AvatarMaskPool` (`engine/core/pool/AvatarMaskPool.{h,cpp}`). It follows
  `AnimationClipPool`:
  - `getOrLoad(path)` with generations, so editor edits invalidate it;
  - it reads through `Data::open`, so resource packs work;
  - it is registered in `Engine::clearPools`/`clearUnusedPools`.

  The loader and the canonical writer live in `engine/core/animation/AvatarMask.{h,cpp}`, and both
  the engine and the editor use them:
  - `parseAvatarMask(text) -> AvatarMaskParseResult{data, warnings, error}`;
  - `writeAvatarMask(data)`;
  - `float AvatarMask::weightOf(scene, root, entity, const pose::ModelRigInfo* rig)`.

### 7.3 The layered evaluator (`engine/core/subsystem/ActionSystemPlayers.cpp`)

`ActionSystem::samplePlayerStates` (:828) branches:
- When `isDefaultLayering` is true, it runs today's code unchanged.
- Otherwise, it calls a new `ActionSystem::evaluateLayeredPlayer(entity, player)`.

`evaluateLayeredPlayer` uses a per-player `anim::PlayerPoseScratch`, held by `AnimationPlayerRuntime`
and reused every frame:
- At bind time, each bound track of each state gets a slot index into a table of
  (target, channel[, element]) slots. Elements are a morph index, a lattice point, or a property key
  (from `anim-properties`).
- For each slot and layer, the scratch keeps the same sums as `TransformBlendAccum` and its
  morph/lattice equivalents: value sum, weight sum, rotation reference.
- There is no hashing per frame.

For each frame:

1. For each layer L, in order. Skip it when its effective weight `λ_L` is 0 (except L = 0) or when
   its mask is loading or failed. `λ_L` is `LayerRuntime::weight`, and 1 for L = 0.
   - Accumulate every sampled state `i` of L (enabled, finishing or held, with weight > 0), as
     `accumulateClip` does, but into L's scratch.
   - The contribution weight is `w_i * trackMask_i[track]`.
   - Additive layers accumulate the delta `d = sample(t) ⊖ reference`:
     - position: `p - p_ref`;
     - rotation: `q_ref^-1 * q`, aligned to the identity's hemisphere;
     - scale: `s / s_ref` per component, where a reference component of 0 gives a ratio of 1 with
       one warning per clip;
     - morph, lattice and float properties: `v - v_ref`;
     - discrete properties: not additive. They are skipped, with one warning per clip.
2. For each slot, compose bottom-up. `R` is the running value. For each layer L that has weight sum
   `S > 0` on the slot:
   - `avg = Σ w v / S`. Rotations are normalized: the sum of hemisphere-aligned quaternions.
   - `α = λ_L * min(1, S)`. Mask and mixing weights are already inside `S`, so a fully masked bone
     has `S = 0` and is skipped.
   - Override, L = 0: `R = lerp(rest, avg, α)`. This is exactly today's rest fill. For a rotation it
     is the same normalized weighted sum: `S*avg + (1-S)*rest` when `S < 1`.
   - Override, L > 0: `R = lerp(R ?? rest, avg, α)`. Positions, scales, morph and lattice use a
     linear lerp. Rotations use a shortest-path slerp (`pose::slerpShortest`).
   - Additive: `R = (R ?? rest) ⊕ α·avgDelta`. Positions, morph, lattice and floats add `α·avgDelta`.
     Rotations: `R * slerp(I, avgDelta, α)`. Scales: `R * (1 + α(avgDelta - 1))` per component.
   - `R ?? rest` means: if no lower layer touched the slot, start from the R2 rest value.
3. Every slot with a value `R` is added to the shared ActionSystem accumulators (`transformBlend`,
   `morphBlend`, `latticeBlend`, properties) with weight 1. Its rest flags stay unset, because the
   rest fill is already applied. `flushPoseBlend` (ActionSystem.cpp:1632) writes it unchanged.
   Anchored UI positions keep the `positionOffset` rule there.

Notes:
- **The rest pose is the floor.** With nothing playing on any layer, nothing is written: today's
  "the last pose stays" rule.
- **Masks are not normalized.** A bone at mask 0.5 on an Override layer at λ 1 gets half the layer
  pose and half the layers below.

### 7.4 Playback semantics changes (`engine/core/animation/AnimationPlayerRuntime.cpp`)

- `playState(player, name, fade, who, PlayMode mode = StopSameLayer)` (:230):
  - With `fade <= 0`, it stops the other states of the target's layer, or of every layer under
    `StopAll`.
  - With `fade > 0`, it fades those same states out.
  - It clears the queue of the target's layer (`StopAll`: every layer's queue).
- `queueState` (:296) waits only for the states playing on the target's layer, and queues on that
  layer's queue. `advancePlayer`'s queue logic (ActionSystemPlayers.cpp:776-825) runs once per layer.
- `stopStates(name = null)`:
  - with no name, it stops every layer and clears every queue;
  - with a name, it stops that state and clears that state's layer queue.
- `blendState`, `rewindStates`, `sample`, `isPlaying`, wrap and held rules (R1b): unchanged.
- **New functions** (`anim::`):
  - `setLayerWeight(player, layer, weight, fade)` clamps to [0, 1], fades linearly, and is ignored on
    layer 0 with a warning once.
  - `layerIndex(player, nameOrIndex)`.
  - `setLayerMask(player, layer, path)` (runtime override; `""` = none).
  - `setLayerBlend`.
  - `stopLayer(player, layer, fade)`.
  - `addMixingTransform(state, path, recursive)` / `removeMixingTransform` / `clearMixingTransforms`.
    All three invalidate the state's `trackMask`.
- **Layer fades** run in `advancePlayer` before the state fades, using the same dt.
- **Events** (`advanceState`, ActionSystemPlayers.cpp:325): add `&& layerEffectiveWeight > 0` to the
  `fires` condition. The layer's effective weight is `λ` (λ is 1 on layer 0).

### 7.5 Edit-mode preview (`ActionSystem::previewPlayer`, :971)

`PlayerPreviewState` gets `int layer`. `PlayerPreview` gets:
- `std::vector<PlayerPreviewLayer> layers` (weight, blend, mask);
- `bool layered`.

The Animation window's "Preview over" mode uses them. When `layered` is set, `previewPlayer` uses the
evaluator: the base state at its time, plus the edited state on its layer at weight 1 with that
layer's mask. When it is not set, the preview behaves as today.

## 8. Phase 1 design: Lua API (`engine/core/script/binding/GameObjectClassesLua.cpp` 1809-1865)

In the signatures below, `layer` is a 0-based integer index or a layer name (one `luabridge::LuaRef`
overload resolves it). An unknown layer warns once and does nothing; getters then return 0, `""` or
-1.

| Member | Semantics |
|---|---|
| `play(name [, fade [, mode]])` | `mode` is `PlayMode.STOP_SAME_LAYER` (default) or `PlayMode.STOP_ALL` |
| `crossFade(name [, fade [, mode]])` | same, with the fade (default `defaultFadeTime`) |
| `queue(name [, fade])` | waits for the playing states of `name`'s layer |
| `getLayerCount()` | ≥ 1 |
| `getLayerName(i)`, `getLayerIndex(name)` | `""` / -1 when unknown |
| `addLayer(name [, weight [, blend [, mask]]])` | appends an authored layer (needsLoad). Returns its index, or -1 when the name is taken. `blend` is `"override"` or `"additive"` |
| `removeLayer(layer)` | not the base layer. Its clip entries move to layer 0 (warning). Returns bool |
| `setLayerWeight(layer, w [, fade])`, `getLayerWeight(layer)` | runtime weight; the getter returns the current, possibly fading, value |
| `setLayerMask(layer, path)`, `getLayerMask(layer)` | runtime; `""` clears the mask |
| `setLayerBlendMode(layer, mode)`, `getLayerBlendMode(layer)` | `"override"` / `"additive"`; runtime |
| `stopLayer(layer [, fade])` | fades out every state of the layer (instant when there is no fade); clears its queue |
| `getAnimator()` on GameObject | alias of `getAnimationPlayer()` |

`AnimationState` gets:
- `layer` (rw): a runtime move. The state keeps its time and weight, and its tracks re-mask.
- `addMixingTransform(path [, recursive=true])`, `removeMixingTransform(path)`,
  `clearMixingTransforms()`. A state with any mixing transforms animates only those paths (weight 1,
  or 0 elsewhere). This multiplies with the layer mask.

The new `PlayMode` enum (`STOP_SAME_LAYER = 0`, `STOP_ALL = 1`) is bound like `CameraType`. C++
mirrors all of it in `engine/core/object/AnimationPlayer.h`: `play(name, fade, PlayMode)`,
`setLayerWeight(int|string, float, float)`, and so on. Lua stubs are regenerated
(`generate_lua_stubs.py`); `-- unresolved:` stays empty.

Garden farmer with Lua only:

```lua
function Farmer:init()
    self.player = GameObject(self.scene, self.entity):getAnimationPlayer()
    RegisterEngineEvent(self, "onUpdate")
end
function Farmer:onUpdate()
    local speed = self.body.linearVelocity:length()
    local want = speed > 0.01 and "Running" or "Idle"
    if not self.player:isPlaying(want) then self.player:crossFade(want, 0.15) end
    local w = self.player:getLayerWeight("Holding")
    local target = self.holding and 1 or 0
    self.player:setLayerWeight("Holding", w + (target - w) * math.min(1, Engine.deltatime * 15))
end
```

## 9. Phase 1 design: editor UX

- **Inspector, Animation Player section** (`editor/window/InspectorWindowClips.cpp`). A **Layers**
  table sits above Clips, collapsed to "Layers (1)" while the player has only the base layer. Each
  row has:
  - a drag handle (reorder; entries follow their layer);
  - Name;
  - Weight (a slider; the base layer shows "1 (Base)" disabled);
  - Blending (Override/Additive; fixed on the base layer);
  - Mask (an asset field filtered to `.mask`, with picker, drop and clear; a missing mask shows
    yellow).
  - The row's context menu has Rename, Remove, Move Up/Down, and **Create Mask from Selection...**.
    That item writes a `.mask` with the selected GameObjects' paths (`children: true`) and assigns it,
    as one scene step that includes the new file.

  Below the table is **+ Add Layer**. Each clip row gets a **Layer** combo.
  - In play mode, the **States** panel groups states under their layer, with a live weight bar and a
    slider that drives the runtime `setLayerWeight` without recording.
  - The Bindings report adds two lines: `mask: N/M bound entities covered` and the mask's warnings.
  - All authored edits are scene steps through `PropertyCmd`.
- **Avatar Mask asset Inspector** (click a `.mask` file):
  - **Default** weight.
  - **Humanoid**: a figure drawn with ImDrawList (head, body, both arms, legs and hands, plus a root
    disc). Clicking a part toggles it 1/0. Green means 1, red means 0, and a tint means a fraction.
    A weight field for the hovered part is in the tooltip. A "Not humanoid" note appears when the
    preview model has no rig.
  - **Transforms**: a tree built from Preview Model (or the selected GameObject: **Use Selection**).
    Each row has a checkbox, a weight drag and a "children" toggle. Shift-click toggles a whole
    subtree.
  - **Users**: the players and layers that use the mask.
  - Edits are project undo steps (clip undo rules). **Assets > Create > Avatar Mask** makes
    `New Avatar Mask.mask` in the current folder, with every part at 1 and default 1.
- **Animation window** (`editor/window/AnimationWindowClip*.cpp`):
  - The clip dropdown groups states by layer (`Base Layer ▸ Walk`, `Holding ▸ Hold`).
  - For a state on a layer above 0, the toolbar shows **Preview over: [None | base states]**. When a
    base state is chosen, the preview composes that base state at the playhead with the edited clip
    on its layer and mask (7.5). Record still keys only the edited clip.
  - Masked-out rows are drawn dimmed, with the tooltip "masked out by Arm.mask on layer Holding".

## 10. Phase 1: MCP and agent surfaces

- `set_properties` / `add_component` on `animationplayer` (Catalog): `layers` (list of
  `{name, weight, blend, mask}`), `layers[i].weight|blend|mask|name`, `clips[i].layer`. Validation:
  - unique layer names;
  - the base layer's weight and blend are refused, with the reason;
  - a mask path must exist and parse.
- `get_animation {entity}` gains `players[0].layers: [{index, name, weight, blend, mask, states: [names]}]`.
  In play mode it adds live `weight` and the queue per layer. The binding report gains
  `mask_coverage: {layer, covered, total, warnings}`.
- `get_animation {mask, root?}` (new mode) returns the parsed mask, its warnings and `users`. With
  `root`, it also returns `coverage: [{path, weight, rule: "transform"|"humanoid:<part>"|"default"}]`
  for every bound entity, capped at 200 with `coverage_total`.
- `write_file` validates `.mask` text (like `.anim`) and refuses unreadable text, returning the
  warnings. Agents write masks with it. There is no new tool for masks.
- `preview_animation` gains `over: <base state>` for the layered preview.
- Guide: `docs/mcp/guide/animation.md` gets a "Layers and masks" section of about 40 lines. The
  `jasper://guide` text (McpInstructions.cpp) gains at most one line pointing to it.
  `docs/mcp/TOOLS.md` is regenerated, and `tools/mcp/tool_budget.py` must not grow by more than
  600 characters in phase 1.

## 11. Phase 2: Animator Controller

### 11.1 Asset `.controller`

```json
{
  "format": "jasperengine.controller",
  "version": 1,
  "guid": "...",
  "parameters": [
    {"name": "Speed", "type": "float", "default": 0},
    {"name": "Watering", "type": "bool", "default": false}
  ],
  "layers": [
    {"name": "Base Layer", "default_state": "Idle",
     "any_state": {"position": [40, 40]},
     "states": [
       {"name": "Idle", "clip": "models/garden/granjero.glb#Happy Idle", "wrap": "loop", "position": [260, 120]},
       {"name": "Running", "clip": "models/garden/granjero.glb#Running", "wrap": "loop", "position": [520, 120]},
       {"name": "Move", "blend_tree": {"parameter": "Speed", "children": [
           {"clip": "anim/walk.anim", "clip_guid": "...", "threshold": 0},
           {"clip": "anim/run.anim", "threshold": 4, "speed": 1}]}, "position": [390, 260]}
     ],
     "transitions": [
       {"from": "Idle", "to": "Running", "conditions": [{"parameter": "Speed", "mode": "greater", "threshold": 0.01}],
        "has_exit_time": false, "duration": 0.15, "fixed_duration": true},
       {"from": "any", "to": "Idle", "conditions": [{"parameter": "Reset", "mode": "if"}], "can_transition_to_self": false}
     ]},
    {"name": "Holding", "weight": 0, "blend": "override", "mask": "anim/arm.mask", "default_state": "Hold",
     "states": [{"name": "Hold", "clip": "anim/holding.anim", "wrap": "loop"}], "transitions": []}
  ]
}
```

- **Parameters.** `type` is one of `float`, `int`, `bool`, `trigger`. `default` is a number or bool;
  a trigger's default is false. Names are unique, case-sensitive Lua-safe strings.
- **States.** Names are unique per layer. A state has `clip` (with `clip_guid` for `.anim` files) or
  `blend_tree`, plus:
  - `speed` (default 1);
  - `speed_parameter` (a float parameter multiplied in);
  - `wrap` (`default` uses the clip's wrap);
  - `cycle_offset` (0..1, the normalized start);
  - `position` (editor only);
  - `mirror`, `foot_ik` and `retarget` (consumed by `anim-retarget`; kept and ignored before that).

  A state with neither `clip` nor `blend_tree` is an **empty state**: it writes nothing (Unity's
  "New State").
- **Blend trees (1D).**
  - `parameter` is a float or int.
  - `children[]` each have `clip`, `threshold`, `speed` (default 1) and `mirror`.
  - `automate_thresholds` (bool): when true, thresholds are spaced evenly over [0, 1].
  - Children are sorted by threshold, and equal thresholds are warned about.
- **Transitions.**
  - `from` is a state name or `"any"`; `to` is a state name.
  - `conditions[]` items are `{parameter, mode, threshold}`. `mode` is one of `greater`, `less`
    (float/int), `equals`, `not_equal` (int), or `if`, `if_not` (bool; triggers use `if`).
  - Fields and defaults (Unity's for new transitions): `has_exit_time` true, `exit_time` 0.75
    (normalized), `duration` 0.25, `fixed_duration` true (seconds; false means a fraction of the
    source length), `offset` 0 (normalized start of the destination).
  - `interruption`: `none` (default), `current_state` or `next_state`.
  - `can_transition_to_self`: `any` transitions only, default true (Unity's default).
  - Order in the list is priority.
- **Layers.** `name`, `weight` (default 1; ignored for index 0), `blend`, `mask` (+ `mask_guid`),
  `default_state` (the first state when absent), `any_state.position` (editor only).

`engine/core/animation/AnimatorController.{h,cpp}` holds:
- `ControllerData` (value form);
- `parseController(text)` and `writeController(data)` (canonical, lenient, unknown keys kept);
- `validateController(data, resolveClip) -> std::vector<ClipWarning>` with the decision 11 checks.

`AnimatorControllerPool` (`engine/core/pool/`) caches immutable `AnimatorController` builds with
generations, like the clip pool.

### 11.2 Runtime

- `AnimationPlayerRuntime` gets a `ControllerRuntime`:
  - `std::shared_ptr<const AnimatorController> controller`;
  - `std::vector<ParamValue> params` (float/int/bool/trigger, initialized from defaults);
  - per layer: `current` (state index), `currentTime`, `transition`
    `{active, to, elapsed, duration, sourceWeights}`, and the list of state groups.
- **States become player states.** Each controller state with a clip becomes one
  `PlayerStateRuntime` named `"<Layer>.<State>"`, on the controller layer's index. Each blend tree
  child becomes a hidden state `"<Layer>.<State>#<child>"`. The existing loading, binding, sampling,
  layers, masks, events and holds are all reused. With a controller, `playerLayers` returns the
  controller's layers.
- **The driver** is `ActionSystem::driveController(entity, player, dt)`. It is called in
  `advancePlayer` before the fades, and sets state weights directly (no fades). For each layer:
  1. **Pick at most one transition per frame**, when no transition is active, or when one is active
     and its interruption source allows it:
     - Check Any State transitions first, in order. A transition whose `to` is the current state is
       skipped unless `can_transition_to_self` is set.
     - Then check the current state's transitions, in order.
     - A transition fires when all its conditions hold and, with `has_exit_time`, when the source's
       normalized time crossed `exit_time` during this step:
       - `exit_time < 1`: checked on every loop at that fraction;
       - `≥ 1`: once, at that value.
     - A transition with no conditions and no exit time fires immediately, with a warning in
       validation.
     - When a transition fires, every trigger in its conditions is reset.
  2. **Start the transition.** The destination starts at `offset × length`, or at its
     `cycle_offset` when `offset` is 0. The duration is in seconds when `fixed_duration` is set,
     otherwise it is `duration × source length`. `onStateExit(layer, from)` and
     `onStateEnter(layer, to)` are queued.
  3. **Weights.** With `x = clamp(elapsed / duration)`:
     - the destination group's weight is `x`;
     - every group that was playing when the transition started keeps its weight at the start times
       `(1 - x)`. This is also what happens when an interrupted transition is frozen.
     - At `x = 1`, the source groups stop and rewind.
     - A zero duration switches instantly.
  4. **Blend tree groups.** Let `p` be the parameter's value.
     - Weights: the child below `p` gets `1 - u`, the one above gets `u` (linear between neighbours,
       clamped to the end children). Everything is multiplied by the group weight.
     - The group's normalized time `τ` advances by `dt × stateSpeed × Σ w_c (speed_c / length_c)`.
       Each child is sampled at `τ × length_c`, wrapped per the state wrap.
     - Events fire only from the child with the highest weight (ties: the lower index).
- **Normalized time.** Normalized time = time / length, with whole loops counted (as Unity reports
  it). Exit times use it.
- **Parameters.** `setFloat(name, v, dampTime)` moves the stored value with `SmoothDamp`
  (critically damped, Unity's `Mathf.SmoothDamp` formula, `smoothTime = dampTime`) once per update.
  It only targets: the next `setFloat` with no damping snaps.
- **Lua with a controller.**
  - `play(name)` jumps instantly: it cancels the layer's transition and starts the state at 0, or at
    `normalizedTime` with `play(name, layer, normalizedTime)`.
  - `crossFade(name, seconds)` starts a manual transition (Unity's `CrossFadeInFixedTime`).
  - `blend` and `queue` log "not available with an Animator Controller" once.
  - `stop()` stops everything. The controller re-enters its default states on the next `play`, or
    when the player is enabled again.
- **Callbacks.** `onStateEnter(layer, name)` and `onStateExit(layer, name)` are queued like events
  and dispatched in `dispatchPlayerCallbacks` (:912) after the pose is written. Each GameObject Lua
  script method `onStateEnter` / `onStateExit` is also called, through the
  `LuaBinding::callLuaScriptMethod` path (:941). This follows the brief's per-object callback
  convention.
- **Start.** On the first update, each layer enters its default state at weight 1 and
  `onStateEnter` fires. `playAutomatically` is ignored, because the Animator always runs.

### 11.3 Lua (phase 2)

These methods are added on `AnimationPlayer`. Without a controller, they warn once and return
defaults.
- Parameters: `setFloat(name, value [, dampTime])`, `getFloat(name)`, `setInteger`/`getInteger`,
  `setBool`/`getBool`, `setTrigger(name)`, `resetTrigger(name)`, `hasParameter(name)`,
  `getParameterType(name)` (`"float"` ...).
  - A type mismatch warns once and converts. Examples: `setBool` on a trigger sets it;
    `setFloat` on an int rounds.
- State info:
  - `getCurrentStateName([layer=0])` returns the state name, without the layer prefix.
  - `getCurrentStateInfo([layer])` and `getNextStateInfo([layer])` return an `AnimatorStateInfo` (a
    copy) with `name`, `fullName` (`"Layer.State"`), `normalizedTime`, `length`, `speed`, `loop`
    and `isValid`. The next-state info is valid only during a transition.
  - `isInTransition([layer])`.
- Playback: `play(name [, layer [, normalizedTime]])`, `crossFade(name, seconds [, layer])`.
- Callbacks (rw functions): `controller` (rw path; setting it reloads), `onStateEnter`,
  `onStateExit`.
- C++ mirrors all of it on `AnimationPlayer`.

### 11.4 Editor: the Animator window (`editor/window/AnimatorWindow.{h,cpp}`, Window > Animation > Animator, `Ctrl+7`)

- **What it shows.** The window shows the controller of the selected GameObject's player, or a
  `.controller` opened from the Project window (double-click).
- **Left panel, two tabs.**
  - **Layers**: a list with add, remove and reorder. A gear per layer edits weight, blending and
    mask.
  - **Parameters**: `+` menu for Float, Int, Bool and Trigger; rename inline; edit the default. In
    play mode the values are live and editable (drag, checkbox, trigger button). Edits there drive
    the runtime and are not saved.
- **Graph canvas** (ImDrawList):
  - Rounded state boxes. The default state is orange, the others grey, and blend trees have a tree
    icon.
  - An Any State box (teal). Arrows for transitions: several transitions between the same pair draw
    one arrow with a "×N" badge.
  - Navigation: pan with the middle mouse or Alt+drag, zoom with the wheel; `F` frames the
    selection and `A` frames all.
  - Right-click on empty space: Create State ▸ Empty / From Clip... / From New Blend Tree.
  - Right-click on a state: Make Transition (then click the target), Set as Layer Default State,
    Copy, Delete.
  - Dropping `.anim` files or model clips onto the canvas creates states.
  - Drag boxes to move them (positions saved); box-select; Delete removes the selection.
  - In play mode, with a GameObject bound, the current state shows a blue progress bar and the
    active transition arrow is animated.
- **Selection inspector.** The right side of the window, and the Inspector when the window has
  focus, shows the selected item:
  - a state: name, motion (clip field or blend tree), speed (+ parameter), wrap, cycle offset;
    mirror and foot IK appear once `anim-retarget` is merged;
  - a transition: has exit time, exit time, fixed duration, duration, offset, interruption, the
    conditions list (parameter combo, mode combo filtered by type, threshold), and a timeline bar
    showing the source and destination overlap (as Unity does);
  - a blend tree: parameter, children (clip, threshold, speed), Automate Thresholds, and a
    weight-curve strip with a scrubbable parameter value that previews in the Scene view.
- **Validation and preview.**
  - The warnings of 11.1 show in a bottom strip. Clicking one selects the item.
  - Edit-mode preview: select a state and press Preview to see its clip (or tree at the scrubbed
    value) in the Scene view through `previewPlayer`.
- **Undo and creation.** Edits are project undo steps, with the clip rules (one drag is one step).
  **Assets > Create > Animator Controller** makes `New Animator Controller.controller` with a Base
  Layer and no states. The Inspector's player section gets a **Controller** field, with **Open**
  (opens the window) and **Create Controller from Clips** (one state per entry, the default clip as
  the default state, entries' layers become controller layers, no transitions; one scene step that
  includes the file).

### 11.5 MCP (phase 2)

- New tool `edit_animator {controller, create?: {} | {from_entity}, ops: [...]}`. It is all or nothing
  and one project undo step (`create.from_entity` is a scene step, like the Inspector button). Ops:
  - parameters: `add_parameter{name,type,default}`, `update_parameter{name, rename?, default?}`,
    `remove_parameter{name}`;
  - layers: `add_layer{name, weight, blend, mask}`, `update_layer{layer, ...}`, `remove_layer`,
    `move_layer{layer,to}`;
  - states: `add_state{layer, name, clip | blend_tree, speed, speed_parameter, wrap, cycle_offset, position}`,
    `update_state`, `remove_state` (also removes its transitions), `set_default_state{layer,state}`;
  - transitions: `add_transition{layer, from, to, conditions, has_exit_time, exit_time, duration,
    fixed_duration, offset, interruption, can_transition_to_self}`, `update_transition{layer, index,
    ...}`, `remove_transition{layer, index}`.

  Positions are optional: the tool lays out new states on a grid.
- The result is the full controller summary: parameters, layers (states with index, transitions with
  index), `warnings` (validation, with JSON paths), and `users`.
- `get_animation {controller}` reads one. `get_animation {entity}` in play mode adds
  `controller: {parameters: {name: value}, layers: [{current, normalized_time, transition: {to,
  progress}?}]}`.
- `set_runtime_property` and `eval_lua` drive parameters in tests:
  `eval_lua "GameObject(...):getAnimator():setFloat('Speed', 2)"`.
- Guide: an "Animator Controllers" section of about 60 lines in `docs/mcp/guide/animation.md`. Tool
  budget: the new tool's definition must stay under 3500 characters. Op details go in the guide.

### 11.6 Phase 2 exported-build path

- Controllers load through `AnimatorControllerPool` with `Data::open`.
- `controller`/`controllerGuid` go through Stream and Factory.
- No code is generated per controller.
- `SceneResources` preloading lists controllers and the clips they reference
  (`collectResourcesInRegistry`).

## 12. Behaviour rules and edge cases

- **L1. Identical results without layers.** A player without layers produces bit-identical poses to
  today's: the fast path, plus the existing `AnimationPlayerTest` suites.
- **L2. Zero weight costs nothing.** An Override layer at λ = 0 is not sampled: no work and no events.
  Its states keep advancing in time (Unity does the same), so raising λ later shows the correct
  phase.
- **L3. Additive with nothing below.** An Additive layer over nothing adds to the rest value.
- **L4. Same clip on two layers.** It is allowed: each entry is its own state.
- **L5. Layers write channels independently.** A channel that one layer animates and a higher layer
  does not passes through unchanged. A higher Override layer that animates only the arm leaves the
  legs exactly as the base produced them, even without a mask.
- **L6. A deleted mask.** The layer contributes nothing, and the error is logged once. Undoing the
  delete relinks the mask (GUID).
- **L7. Renaming a layer.** The Inspector rename updates nothing else, because entries reference the
  layer by index. In Lua, layer names resolve on each call.
- **L8. Reordering layers.** The Inspector moves entries with their layer: one PropertyCmd step that
  changes `layers` and every affected `clips[i].layer`.
- **L9. Masks need a humanoid rig.** A humanoid mask on a non-humanoid root falls back to transforms,
  then `default`, with a binding warning. A model that is still loading delays the mask resolution,
  like bindings (R10).
- **L10. Finishing and queues per layer.** `onFinish` for a state on a zero-weight layer still fires,
  because finishing is about time, not weight. Its events do not fire. Queues advance per layer
  independently.
- **L11. Controller start.** A controller state referring to a missing clip is an empty state, with an
  error once. With a controller set and a non-empty `clips`, the clips are ignored, with a warning
  once per player.
- **L12. Lifetime.** A trigger set in the same frame a transition consumed it stays set for the next
  frame only if `setTrigger` is called after the controller update, so callbacks count as "next
  frame". Parameters reset when the player's runtime resets (play stop, scene load). Nothing
  outlives the owner's scene.
- **L13. Disabled player.** With `enabled = false` (R25), time stops, nothing is written, and
  parameters keep their values.
- **L14. Time scale.** Layer fades and transitions use the scene dt, which is scaled. The game's
  `anim.speed = 1/Time.timeScale` maps to `player.speed`.

## 13. Performance (mobile)

- **Budget.** On a mid-range Android device (Cortex-A76 class), 40 characters with 65 bones each, two
  layers (one masked Override), one crossfade on the base and one controller each must cost
  ≤ 1.5 ms per frame in `ActionSystem::updatePlayers`. A test on the host checks
  ≤ 0.6 ms for that scenario in a Release build.
- **No per-frame allocation or hashing.** Scratch arrays are sized at bind, and mask weights are
  cached per track.
- **The evaluator is skipped for default-layered players.** Override layers at λ = 0 cost nothing
  beyond their time advance.

## 14. Tests

Engine tests (`engine/tests/anim`, target `jasperengine-anim-tests`) build scenes in code: a root, a
child "Arm" with "Hand", and a child "Leg". Two-key clips: `walk` moves Leg from x=0 to x=2 over 1 s
and rotates Arm 0→90° about Z; `hold` sets Arm to 45° about X and Hand y=1.

1. **Fast path.** The default-layered player is bit-identical to the pre-change code. Rerun the full
   existing `AnimationPlayerTest`; add a recorded checksum of 100 sampled frames of a crossfade.
2. **Override layer with a mask.** Layer 1 Override, weight 1, transform mask `{Arm, children:
   true}`. Base plays `walk` and layer 1 plays `hold`. At t=0.5:
   - Arm rotation is exactly `hold`'s (45° X, quat dot within 1e-5);
   - Leg.x = 1.0 (±1e-5);
   - Hand.y = 1.
3. **Layer weight 0.5.** Arm = slerp(walk(0.5) = 45° Z, hold = 45° X, 0.5), within 1e-5. Leg is
   unchanged.
4. **Fractional mask.** Mask weight 0.25 on Arm, layer weight 1: Arm = slerp(base, hold, 0.25).
5. **Additive.**
   - A clip `nod` has Arm at 0° then 30° X, with `additive_reference` 0. Over base `walk` at t=1
     (90° Z), the additive at weight 1 gives `R = q(90°Z) * q(30°X)` (1e-5).
   - Position additive: `+(p − p_ref)`. Scale additive: a ratio.
   - At weight 0.5 the delta is `slerp(I, Δ, 0.5)`.
6. **Per-layer stop.** `play("hold")` on layer 1 does not stop `walk` on layer 0. `play("walk", 0,
   PlayMode.STOP_ALL)` stops both.
7. **Per-layer queue.** `queue("hold2")` on layer 1 starts when `hold` (once, 0.5 s) ends, while
   `walk` loops on. The start time is checked to within one frame's offset (R17 precision).
8. **Events.**
   - Layer 1 at weight 0: `hold`'s events do not fire; at 0.01 they do.
   - Base events always fire.
   - Masked-out tracks do not affect events.
9. **Mixing transforms.** `state:addMixingTransform("Arm")` on a base-layer state: Leg goes back to
   rest, and Arm animates.
10. **Humanoid mask.** Use `engine/tests/pose/TestRigs.h` `tposeRig` as an entity hierarchy. The mask
    has `right_arm: 1` and default 0. Check:
    - right shoulder, upper arm, lower arm and hand are covered;
    - right fingers are not (part `right_fingers` is unset, so default 0);
    - a twist bone under the right upper arm is covered;
    - `left_hand` is not covered;
    - `root` covers the glTF root node.
11. **Mask IO.**
    - Round trip is canonical (byte-identical).
    - Leniency: a bad entry is dropped with its JSON path.
    - Unity YAML is refused.
    - `true`/`false` read as 1/0.
12. **Layer runtime survives a clip list edit.** After `needsLoad`, the weights are kept by name.
13. **Controller (phase 2).**
    - Parse and canonical write. Validation flags the GranjeroAnim port's dead end: the `Watering`
      state has no outgoing transition and its clip does not loop. A parameter that no transition
      reads is reported as "unused". At runtime, a `setBool` on a trigger logs a warning (the
      MonsterAnim `VictoryDance` bug).
    - Transitions:
      - `Speed=0.02` moves Idle to Running over exactly 0.15 s. At 0.075 s, the weights are 0.5/0.5
        (1e-5).
      - A trigger is consumed after it fires.
      - Exit time 0.9 with a 1 s loop fires at t ≈ 0.9 (±one step), not before.
      - An Any State `can_transition_to_self=false` transition does not retrigger.
      - Interruption `next_state` lets the destination's transition take over during a transition.
    - Blend tree: thresholds 0 and 4, `Speed=1`: weights 0.75/0.25. τ speed is a weighted average of
      the inverse lengths (clips 1 s and 0.5 s: dτ/dt = 0.75/1 + 0.25/0.5 = 1.25).
    - Callbacks: `onStateEnter`/`onStateExit` order and arguments, and the Lua method call.

Editor tests (`editor/tests`, `jasperengine-editor-tests`):
- Stream round trip of layers, entry layer, mask and controller GUIDs; prefab override of
  `layers[1].weight`.
- Factory output compiles in the generator test.
- PropertyCmd undo of a reorder (L8).
- Avatar Mask Inspector: the figure's toggle writes the right part (driver-based, like
  `AnimationWindowClipTest`).
- The Animator window driver: create a state, make a transition, set a condition, undo, redo.

MCP tests (`tools/mcp/tests/test_animation_layers.py`, `test_animator_controller.py`):
- The farmer flow: `write_file` the mask; `set_properties` layers; play; `eval_lua setLayerWeight`;
  `get_runtime_entity` on the right forearm equals the hold pose, while the left forearm equals the
  base pose.
- Controller flow: `edit_animator` builds GranjeroAnim. Play; set Speed with `eval_lua`;
  `get_animation {entity}` shows `current: "Running"` after 0.2 s.
- `get_animation {mask, root}` coverage.
- The tool budget check.

## 15. Acceptance criteria

- [ ] All existing animation tests pass unchanged. Scenes saved before the change load and play
  identically (test 1).
- [ ] Tests 2 to 12 pass (phase 1). Test 13 and the editor and MCP controller tests pass (phase 2).
- [ ] The Garden farmer case works in play mode and in an exported Linux build:
  - layers with Lua (phase 1);
  - a `.controller` (phase 2).

  The right arm holds while the legs run, and the layer weight lerps.
- [ ] The Inspector, Avatar Mask Inspector, Animation window "Preview over" and the Animator window
  work with undo. A Unity `.mask` or `.controller` file shows the refusal message.
- [ ] Lua stubs are regenerated with no `-- unresolved:`. The `lua-api.md` AnimationPlayer section
  documents every new member.
- [ ] `docs/animation-clips.md` gains "Layers and masks" and "Animator Controllers" sections, and the
  "Not in this version" list is updated. `docs/unity-glossary.md` gains rows: Animator → Animation
  Player + Controller; AvatarMask → Avatar Mask; layer weight / blending; `AddMixingTransform`;
  `SetFloat`... `docs/mcp/guide/animation.md` is updated, and `TOOLS.md` is regenerated within
  budget.
- [ ] The performance test meets 13.

## 16. Integration points

- `engine/core/component/AnimationPlayerComponent.h:38-80`: entry `layer`, `layers`, `controller`,
  the callbacks.
- `engine/core/animation/AnimationPlayerRuntime.h:100-207`: `LayerRuntime`, per-layer queue,
  `trackMask`, the scratch. `AnimationPlayerRuntime.cpp:230` (`playState`), `:270` (`blendState`),
  `:296` (`queueState`).
- `engine/core/subsystem/ActionSystemPlayers.cpp`:
  - `:113` `resolveBindings` (slot indices, mask weights);
  - `:313-412` `advanceState` (events gated by layer weight);
  - `:767-826` `advancePlayer` (layer fades, per-layer queues, the controller driver);
  - `:828` `samplePlayerStates` (fast path vs evaluator);
  - `:912-951` dispatch (`onStateEnter`/`onStateExit`);
  - `:971` `previewPlayer` (layered preview).
- `engine/core/subsystem/ActionSystem.h:62-103` (accumulators), `ActionSystem.cpp:1632`
  (`flushPoseBlend`, unchanged rules).
- `engine/core/animation/AnimationClip.h` / `AnimationClipIO.cpp`: `additive_reference`, in the known
  top-level keys at :1321.
- New files:
  - `engine/core/animation/AvatarMask.{h,cpp}`, `engine/core/pool/AvatarMaskPool.{h,cpp}`;
  - `engine/core/animation/AnimatorController.{h,cpp}`, `engine/core/pool/AnimatorControllerPool.{h,cpp}`;
  - `editor/window/AnimatorWindow.{h,cpp}`.
- `engine/core/pose/ModelRig.h:112` `buildModelRig` (humanoid part resolution).
- `engine/core/script/binding/GameObjectClassesLua.cpp:517` (`getAnimator` alias), `:1809-1865`.
- `editor/Stream.cpp:4800, 5343, 7365`; `editor/Factory.cpp:1916, 2991`; `editor/Catalog.cpp`
  (animationplayer property table); `editor/window/InspectorWindowClips.cpp`;
  `editor/window/AnimationWindowClip*.cpp`; `editor/mcp/tools/AnimationClipTools.cpp`
  (get_animation modes, write_file validation hook); `editor/mcp/McpInstructions.cpp`.
- `editor/project/ProjectPrefabPaths.cpp:169` (GUID follow-up for `.mask` and `.controller` like
  `.anim`); the AssetDatabase GUID registration for the two new extensions.

## 17. Cross-feature dependencies

- **`anim-properties` (#12)** adds property slots to the evaluator's slot table. Merge order:
  `anim-layers` first, then `anim-properties`, then `anim-retarget`. Each later branch rebases on the
  earlier one.
- **`anim-retarget` (#11)** adds humanoid playback. Its slot rotations enter this evaluator as normal
  rotation contributions, so masks and layers apply to them. The controller state flags `mirror`,
  `foot_ik` and `retarget` become live there. The humanoid part resolution (6.2 rule 2) shares its
  rig lookup.
- **`script-lifecycle` (#17)**: `onStateEnter`/`onStateExit` use the same per-object method dispatch
  as animation events (brief convention). No dependency in code order.
- **`physics-3d` (#9) ragdoll**: disabling the player (`enabled = false`) hands bones to physics, as
  today. Masking the ragdoll's body parts is a possible follow-up.

## Implementation notes

Decisions taken where the spec was silent or ambiguous, deviations, and the extension points the
later animation features plug into. Code references are to the `gaps/anim-layers` branch.

### Engine runtime (phase 1)

- **Files.** The evaluator is `engine/core/subsystem/ActionSystemLayers.cpp` (the spec named
  ActionSystemPlayers.cpp); helpers both files use are in `ActionSystemPlayersDetail.h`. The two
  asset pools share one template, `engine/core/pool/JsonAssetPool.h`. `.anim`, `.mask` and
  `.controller` share the JSON plumbing in `engine/core/animation/AnimJson.{h,cpp}` (the clip parser
  now calls its document parse: same errors, same positions).
- **Fast path (L1).** `isDefaultLayering` is false when the component or the runtime has more than
  one layer, a mask on layer 0 (authored or `setLayerMask`), a state with mixing transforms, or a
  controller. Default players run the unchanged v1 loop. Test 1 records the hash of 100 frames of a
  crossfade computed by the same test built against `main` before this feature
  (`0x79e15195f729cd77`); the branch reproduces it bit for bit.
- **Masked-out bones of the base layer go to their rest value** (decision 2, test 9): a bound
  position/rotation/scale track whose mask weight is 0 on layer 0 still marks its slot, which then
  composes to `lerp(rest, -, 0) = rest`. Morph and lattice tracks are not reset that way (writing
  every morph target's rest would fight other writers). On layers above 0 a masked track is simply
  skipped (S = 0).
- **Authored layer edits in play mode.** The runtime rebuild (needsLoad) keeps each surviving
  layer's runtime weight, blend and mask by name, unless the authored value changed since the last
  build: an Inspector or MCP edit applies at once, a Lua `setLayerWeight` survives a clip list
  edit (test 12).
- **Mask resolution stamps.** A state's per-track mask weights are recomputed when its layer, the
  layer's mask (pointer), the slot table (`AnimationPlayerRuntime::scratchSerial`, rebuilt after
  any rebind), its mixing transforms or the humanoid rig change. A mask's transform entries are
  resolved again with each slot table rebuild, not on unrelated hierarchy changes.
- **Additive deltas use the raw track values** (`sample ⊖ reference`): for relative tracks the
  rest cancels out, so the delta is the same. A zero scale component at the reference gives a ratio
  of 1, warned once per state (the spec said per clip).
- **A layer whose mask failed to load** contributes nothing, logs once and retries every second
  (the clips' pace); the base layer included (then the player writes nothing).
- **Shared pose blend.** The evaluator writes its composed values into the shared
  `TransformBlendAccum` map with weight 1, as specified. To meet the budget the map now keeps its
  nodes between frames (`TransformBlendAccum::reset`; an entry no one wrote this frame is dropped
  by the flush, so only what animates every frame keeps its node): no allocation per animated
  entity and frame. Results are unchanged (test 1). A rebind of any state rebuilds the player's
  whole slot table (reusing its vectors); with unresolved paths that is at most 4 times a second.
- **Lua layer arguments** are bound as two overloads (an integer index first, then a name), so the
  stubs show `integer` and `string` forms.

### Animator Controller (phase 2)

- **Player states of a controller** are `"<Layer>.<State>"` and, per blend tree child,
  `"<Layer>.<State>#<i>"` (the child's index). They are `driven`: the driver owns their time
  (`drivenTime`), weight and events; they never advance or finish by themselves, and `onFinish`
  does not fire for them (Unity's Animator has no such callback).
- **Interruption.** With `none` nothing interrupts a running transition, Any State transitions
  included (the spec: "when one is active and its interruption source allows it"). With
  `current_state` or `next_state`, Any State transitions are checked first, then the source's or
  the destination's, in order; a transition to the running destination is skipped. Interrupted
  sources keep playing (their times advance) at their frozen weights × (1 - x).
- **Callbacks.** When a transition fires, `onStateExit` names the state the layer last entered
  (the abandoned destination when a transition is interrupted), then `onStateEnter` the new
  destination: every entered state gets exactly one exit. `play` and `crossFade` from Lua send the
  same pair. The Lua script methods get the layer as an integer
  (`LuaBinding::callLuaScriptMethod(..., integers = true)`).
- **Exit time below 1** is checked at every `k + exit_time` (k ≥ 0) crossing, for non-looping clips
  too (their normalized time keeps growing past 1, as in Unity). An empty state counts 1 second as
  its length.
- **`play(name, x)` with a controller** reads `x` as the layer (Unity's `Animator.Play(name,
  layer)`), and `play(name, a, b)` as layer and normalized time. In Lua whole-number arguments go
  through the `PlayMode` overload, which the player routes to the controller form;
  `crossFade(name, seconds, layer)` likewise.
- **Loading.** Parameter and state calls made before the first update (from `init`) load the
  controller synchronously (AnimatorControllerPool, Data::open). A reload after an edit keeps each
  layer's current state by name and normalized time, and the parameters by name and type.
- **`stop()`** stops the controller until the next `play`, or until the player is disabled and
  enabled again (then the default states are entered). A controller that was not stopped resumes
  where it was after a disable (L13).
- **Validation details.** Any State transitions count as exits for the dead-end check; empty
  states are never dead ends; a blend tree loops when all its children loop. "Unreachable" is a
  graph search from the default state (Any State targets are reachable).
- **Re-entering a playing state** (an Any State transition to the current state with
  `can_transition_to_self`, a crossFade to the playing state, an interruption back to a source)
  restarts it and keeps the weight it had on top of the transition's `x` (there is no copy state,
  as in the legacy player): no dip toward the rest pose, but no blend between its two times.
- **Events and transitions.** The events of the state that is not fading out are collected over
  the step before a transition is picked, so the step that crosses an exit time still fires the
  source's events. A state entered through a transition fires its start-time events on the first
  update it has weight. Triggers read by fired transitions are reset after every layer was driven,
  so a trigger two layers read fires on both.
- **Time.** Transitions run in the player's time (`player.speed` scales them, as `Animator.speed`
  does). A non-fixed duration is a fraction of the source's length in the source's own time
  (divided by its speed).
- **A state whose clip failed to load** is an empty state (L11): 1 s long, so its exit times
  still come. Blend tree children without weight or with a failed clip don't count toward the
  tree's cycle.
- **A reload in the middle of a transition** (a controller edit, a clip list change) completes it:
  the layer keeps the destination.
- **Legacy calls on controller states.** `stopLayer` and `state.layer` warn once and do nothing
  with a controller (the driver owns its states). The three-number Lua forms
  `play(name, a, b)` / `crossFade(name, a, b)` are one binding that reads the numbers by whether the
  player has a controller (`layer, normalizedTime` / `seconds, layer`) or not (`fade, PlayMode`);
  the C++ API has distinct overloads.
- **Mismatched parameter types** warn once per (parameter, requested type) and convert:
  `setBool` on a float sets 0 or 1, `setFloat` on an int rounds, `setBool` on a trigger sets it.

### Editor and MCP

- **Documents.** `.mask` and `.controller` edits go through one template,
  `editor/animation/AnimAssetDocument.h` (`AvatarMaskDocument`, `AnimatorControllerDocument`),
  modeled on AnimationClipDocument: one project undo step per commit, merge keys for drags, reload
  on outside changes, pool invalidation and player reload on every revision.
- **References.** Both assets are GUID-tracked JSON assets. A `.controller` names its clips and
  masks by path (the runtime loads by path; GUIDs are editor-only), so renaming a clip, model or
  mask also rewrites the controllers that name it, in the same undo step. Mask and controller
  "users" include the layers of the project's controllers, not only players in scenes. A player
  with a controller preloads the controller's clips and masks instead of its own clip list.
- **Layer edits** (`editor/command/type/PlayerLayerActions.h`) are single scene steps that move
  `layers` and every affected `clips[i].layer` together (L8). The base layer can't be removed or
  moved and nothing moves into index 0; a list back to the default Base Layer is stored empty. An
  edit on a player without authored layers writes the whole list.
- **`check --anims`** treats a model sub-clip as `once` (the importer's wrap) for the dead-end
  check; `.mask`/`.controller` count toward the `anims` total.
- **Inspector.** The Avatar Mask figure's tooltip shows the hovered part's weight (ImGui tooltips
  can't hold a field); a fraction is set with the slider under the figure for the last-clicked
  part. The preview model treats every glTF node as a bone for humanoid detection and honours the
  `.humanoid` sidecar; a prefab preview uses the first model it names. Create Mask from Selection
  and Create Controller from Clips ask for the file with a save dialog (`<Layer>.mask`,
  `<GameObject>.controller`). For a player with a controller, the Animation window still lists the
  player's own clip entries; controller clips are opened from the Project window or the Animator
  window.
- **Animator window.** Its selection inspector is the window's right panel: the Inspector has no
  hook for another window's selection. A state drag commits once on release (one undo step). A new
  blend tree on a controller without a number parameter creates a float "Blend" parameter (Unity's
  behaviour). New Any State transitions start without an exit time. Removing a parameter drops the
  conditions that read it and clears the blend trees and speed parameters that use it. States
  without a position (hand-written files) show on a 4-column grid and get one when first moved.
  Following the selection binds to the nearest ancestor player with a controller.
- **MCP.** `edit_animator` op paths are project-relative (stored asset-relative with their GUIDs).
  `preview_animation` refuses players with a controller (use the Animator window's preview) and
  `over` on base-layer states. `create.from_entity` and the Inspector's Create Controller from
  Clips share one scene step (`createControllerFile` in `editor/window/InspectorAnimLayers.h`).
  Tool budget: phase 1 grew `get_animation` and `preview_animation` by 212 characters (limit 600),
  `edit_animator` is 2290 (limit 3500); `TOTAL_LIMIT` was raised by the measured 2502 with a
  comment in `tools/mcp/tests/test_html_ui_tools.py`.

### Performance (spec 13)

- **Occlusion.** Before sampling, the evaluator sums, per single-element slot, the state weights ×
  track masks of every Override layer at weight 1; where that reaches 1, the layers below do not
  sample the slot (the layer's value replaces it exactly: a full override now writes the layer's
  average directly). In the budget scenario the base walk/run tracks of the masked arm bones are
  skipped. `Quaternion`'s copy constructor, assignment and plain constructors are inline (same
  arithmetic, bit-identical results; test 1 still matches main's hash).
- **Measured** (`Layers.PerformanceBudget`: 40 characters × 65 bones, a controller each with a
  base crossfade and a masked Override layer, the whole `ActionSystem::update`, best of 8 × 30
  frames): 0.52–0.56 ms on the 8-core host under heavy load; 0.597–0.608 ms on the shared 2.0 GHz
  EPYC build box (load 20–30, beside an MCP suite). Like `CanvasGroupTest.PerformanceBudgets`, the
  0.6 ms assertion is skipped when the load average exceeds the core count (the time is still
  printed); on the box it sits at the line, which main's own Canvas Group budget test also
  missed there. The mobile figure (1.5 ms on a Cortex-A76) was not measured on a device.

### Extension points

- **anim-properties (#12).** `playerdetail::buildPoseSlots` skips `ClipChannel::Property` tracks
  today: a property track becomes a `PoseSlot` with `channel = Property`, `key` naming its property
  and `width` its component count (elements hold up to 4 floats; wider values take several
  elements). `composePlayerLayers` composes continuous elements with the linear rules (Override
  lerp, Additive `+ α·Δ`); discrete properties need their own winner-takes-all element kind and
  are skipped on Additive layers (the reference sample is there: `PlayerStateRuntime::reference`).
  The write-out (step 3) is where a `PropertyBlendAccum` is fed.
- **anim-retarget (#11).** `AnimationPlayerRuntime::humanoid` (`anim::HumanoidSlots`, filled by
  `anim::resolveHumanoidSlots`, the spec 6.2 rig rule) is the target-rig lookup to share;
  `humanoidRevision` tells when it changed. Humanoid contributions enter as ordinary
  Rotation/Position slots of the bone entities: a realisation step fills the state's samples
  before `composePlayerLayers` (one `PoseStateInput` per state). `ControllerState::mirror`,
  `footIK` and `retarget` (and `BlendTreeChild::mirror`) are parsed, kept and written back, and
  ignored until then.
