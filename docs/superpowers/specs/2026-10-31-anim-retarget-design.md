# Humanoid retargeting (`anim-retarget`, ledger #11)

Status: specced on 2026-10-31. Branch: `gaps/anim-retarget`. It merges after `anim-layers` (#10) and
`anim-properties` (#12) and rebases on them (see section 16).

Read first:
- [AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) (its shared conventions are binding);
- [docs/pose-animation.md](../../pose-animation.md) and the pose specs `2026-10-23-pose-animation-phase*`
  (PoseRig, the humanoid map, the neutral pose);
- [docs/animation-clips.md](../../animation-clips.md) and [2026-10-24-animation-clips-design.md](2026-10-24-animation-clips-design.md);
- [2026-10-31-anim-layers-design.md](2026-10-31-anim-layers-design.md) (the layered evaluator).

## 1. Goals

- Retargeting: a clip authored on one humanoid rig plays correctly on any other humanoid rig. This
  holds across bone names (Mixamo, Unreal, Blender/Rigify, VRM, Unity), rest poses (T-pose or A-pose)
  and proportions. It works in editor Play mode and in exported builds.
- **Humanoid clips** are the runtime format, like Unity's muscle clips. A `.anim` holds
  rig-independent per-slot joint rotations relative to each rig's neutral pose. It also holds a
  normalized hips (body) position and foot goals. Any Animation Player plays it on any humanoid
  character it controls.
- Three ways to get one:
  - **Model import setting** "Animation Type: Humanoid", stored in the existing `.humanoid` sidecar
    next to the model (Unity's Rig tab). Every `model.glb#Clip` of that model is served as a
    humanoid clip.
  - **Per player entry** `retarget: true`. It converts a generic clip on load, from its source model:
    the model of a model clip, or the `source` / `preview.model` of a `.anim`.
  - **Convert to Humanoid Clip...**, which writes an editable humanoid `.anim`. It is in the editor
    and in MCP.
- The existing pose system does the mapping and the maths: PoseRig's neutral pose, its canonical
  frame and its joint rotations. Retargeting reuses them, so T-pose and A-pose differences cancel
  out.
- **Fingers, eyes and jaw**: a new extra-bone map (Unity's other 33 humanoid bones) with automatic
  detection and manual fix-up.
- **Avatar fix-up UI**: an Avatar section in the Model Inspector and an **Avatar Configuration**
  window with:
  - an automatic mapping and a per-slot manual mapping;
  - twist corrections;
  - a neutral-pose and test-clip preview.
- Playback options per state: **Mirror** and **Foot IK**. Foot IK pins the feet to the source's
  scaled foot goals and fixes foot sliding when leg proportions differ.
- Humanoid clips blend with generic clips, layers and masks (#10) and property tracks (#12).

## 2. Non-goals

- Reading Unity muscle `.anim` files. The game's `Holding.anim` (5 muscles) and `Watering.anim`
  (130 muscle curves) must be re-authored or re-exported as glTF. Converting Unity muscle space is a
  possible follow-up.
- Root motion: moving the GameObject from the clip. The hips carry the motion as today.
- Unity muscle limits as authored ranges (the pose system already has joint limits for poses).
  Stretch, twist distribution settings (`armTwist`...), IK hints, look-at, and Unreal-style chain
  retargeting for non-humanoid rigs (quadrupeds, tails).
- Retargeting entity Animations (scene keyframe tracks) or vertex animations directly. Bake a clip
  first. The Vertex Animation baker can bake a retargeted Animation Player state, because the player
  plays it.

## 3. Users and stories

- **Porter (human).**
  - Imports `mrbeast.glb` (Mixamo) and `idleanim.glb` (an Unreal-named rig).
  - Sets idleanim's Animation Type to Humanoid in the Model Inspector.
  - Lists `idleanim.glb#Idle` in MrBeast's Animation Player. It plays retargeted, with the arms
    correct, the hips at MrBeast's height and the feet planted.
- **Designer.**
  - A downloaded character's forearms look twisted. The designer opens **Configure Avatar...**,
    previews the walk, drags the forearm twist correction until the wrists look right, and saves.
  - Every humanoid clip and pose animation on that model is fixed.
- **Animator.** Keys a wave on the Hero in the Animation window, then runs **Convert to Humanoid
  Clip...**. The Robot plays the converted clip, and the Animation window edits it as humanoid rows
  with the angles raise, forward and twist.
- **AI agent.**
  - Converts with `edit_animation {clip: "anim/wave_h.anim", create: {from: "anim/wave.anim", humanoid: {}}}`.
  - Reads `retarget_report` (the slot mapping on both rigs and the scale ratio).
  - Verifies numerically with `get_animation {clip, root: "Robot", times: [0.5]}`, which returns the
    realised local rotations and the joint angles.
  - Fixes a bad map with `set_humanoid`.

## 4. Research summary

- **Unity Mecanim.**
  - References: the
    [HumanBodyBones](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/HumanBodyBones.html)
    list (55), [HumanPose](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/HumanPose.html)
    (bodyPosition, bodyRotation, muscles),
    [humanScale](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Animator-humanScale.html),
    [Configuring the Avatar](https://docs.unity3d.com/2022.3/Documentation/Manual/ConfiguringtheAvatar.html).
  - **Take:**
    - retarget at play time from a rig-independent clip format;
    - the model-level Animation Type: Humanoid setting;
    - automatic mapping with a manual fix-up;
    - body position normalized by human scale (which fixes hips height and stride);
    - Foot IK and Mirror as state options.
  - **Leave:**
    - The 95 muscles with their authored ranges. The pose system's joint rotations relative to the
      neutral pose already exist, are tested, and can be edited as angles.
    - "Enforce T-Pose". Our neutral pose is computed from bone directions, so any rest pose works.
- **Godot 4.**
  - References: [retargeting 3D skeletons](https://docs.godotengine.org/en/stable/tutorials/assets_pipeline/retargeting_3d_skeletons.html),
    [SkeletonProfileHumanoid](https://docs.godotengine.org/en/stable/classes/class_skeletonprofilehumanoid.html).
  - **Take:**
    - a profile of named slots, with the bone map stored as an import resource (our `.humanoid` sidecar);
    - "Normalize Position Tracks" (hips motion divided by hips height, then multiplied back on the
      target);
    - "Overwrite Axis" (rotations expressed in a profile reference frame, which is our canonical
      frame);
    - "Fix Silhouette" (our neutral pose).
  - **Leave:** Godot rewrites the rests at import time, which changes the meshes. We keep the model
    untouched and convert per frame.
- **Unreal IK Retargeter.**
  - References: [IK Rig retargeting](https://dev.epicgames.com/documentation/en-us/unreal-engine/ik-rig-animation-retargeting-in-unreal-engine),
    [FK chain settings](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/RetargetFKChainSettings),
    [root settings](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/RetargetRootSettings).
  - **Take:**
    - pelvis translation scaled by the ratio of the pelvis heights (our humanScale);
    - IK goals for the feet so they do not slide;
    - a retarget pose edit for odd rigs (our twist corrections).
  - **Leave:** chain-based interpolated FK. Humanoid slots with an exact composition rule
    (section 7.3) are simpler and are deterministic per slot.
- **Bake at import versus runtime.**
  - Baking a converted `.anim` per (clip, target rig) pair multiplies the assets (N clips × M rigs)
    and goes stale when the source changes.
  - Runtime conversion from a humanoid clip costs a few quaternion products per slot per frame.
  - **Decision: runtime humanoid clips.** An optional bake to a file (Convert) covers editing and
    agents.

## 5. Product decisions

1. **One rig-independent format: humanoid tracks inside `.anim`.** A clip with any humanoid track is
   a **humanoid clip**. The file version is 2, because v1 readers drop unknown channels (see
   AnimationClipIO.cpp:982), and the version bump stops an old editor from silently losing data.
   Files without humanoid tracks are still written as version 1, byte-identically.
2. **Slot rotations are stored as `E`**: the pose system's joint rotation of the slot relative to the
   neutral pose, in the canonical frame (PoseRig: `local = left * E * neutral`, PoseRig.cpp:344). `E`
   is a quaternion. The editor and MCP show it as the slot's three DOF angles (raise, forward and
   twist, in the pose system's names). `{"angles": [...]}` is accepted as input.
3. **The body (hips) position is stored in the canonical character frame** as (left, up, forward),
   relative to the rig's neutral (grounded) hips, in humanScale units. The target places its hips at
   `neutralHips_t + C_t * (offset * humanScale_t)`. This fixes hips height and stride length.
4. **Missing slots.**
   - When the target lacks a slot the clip has, that slot's `E` is folded into the next present
     descendant slot. The rule is exact, because `E_b = G_p^-1 G_b` composes along chains (7.3).
   - When the clip lacks a slot the target has, the target's slot stays neutral (`E = I`).
5. **Extra bones** cover fingers (30), eyes (2) and the jaw. They are a new map of their own, beside
   the 22 slots. The 22-slot pose contract is not touched.
   - Extra rotations are stored as the delta from the bone's rest, expressed in the canonical frame
     (7.4), so bone-axis conventions cancel out.
   - Bones with neither a slot nor an extra mapping stay generic path tracks. They bind by path, or
     by unique bone name for humanoid clips (7.6).
6. **The Avatar is the existing `.humanoid` sidecar, extended.** There is no new component and no new
   asset type. New fields:
   - `animation_type` (`generic` by default);
   - `extra_bones` overrides;
   - `twist` corrections in degrees, which rotate the slot's neutral pose about its neutral direction.

   A sidecar with only `bones` is still written as version 1, byte-identically. Twist corrections
   also apply to pose animations on that model, which is consistent: one rig, one neutral.
7. **Backward compatibility.** No existing clip is ever retargeted by surprise. Today a generic clip
   shared by two same-named rigs plays absolute local rotations, and it keeps doing so. Retargeting
   happens only when one of these is true:
   - the clip is humanoid;
   - the clip's source model is marked Humanoid;
   - the entry says `retarget: true`.
8. **Conversion runs in the clip pool** (engine side, so it works in exported builds). It runs on the
   thread pool's async load when async loading is on, like model sub-clips.
   - It needs the source skeleton. The skeleton is built from the glTF data
     (`pose::buildSkeletonFromGltf`, new), so no scene is needed.
   - Its rig is cached by model path plus sidecar stamp, like `PoseRigCache`.
9. **The target rig** is the first ModelComponent on or under the player's root whose
   `buildModelRig` succeeds. This is the same lookup as the humanoid masks of `anim-layers`.
   - Until it is ready, humanoid states wait, like the existing model-load wait (R10).
   - If no rig works, humanoid tracks are unresolved. The binding report says "not a humanoid: Map
     Bones...", and generic tracks of the clip still play.
10. **Mirror and Foot IK are per-entry flags** (`mirror`, `footIK`), and per controller state in
    `anim-layers` phase 2. They only affect humanoid playback.
    - Foot IK runs per state sample, before the layered blend. It is an approximation of Unity, which
      applies IK on the layer result. The order is documented.
11. **Humanoid clips stay editable.**
    - The Animation window shows a row per slot as "Humanoid : Left Upper Arm", with the angle
      curves.
    - Record keys the `E` of a mapped bone that you rotate. You need to bind to a humanoid player
      for this.
    - Curve editing uses the angle components (raise, forward, twist), converted to `E` on write.
      Interpolation stays a quaternion slerp or Hermite curve.

## 6. File formats

### 6.1 `.anim` version 2: humanoid tracks

```json
{
  "format": "jasperengine.anim",
  "version": 2,
  "guid": "…",
  "frame_rate": 30,
  "wrap": "loop",
  "humanoid": {"source_model": "models/hero_animated.gltf", "human_scale": 0.982},
  "tracks": [
    {"channel": "humanoid", "bone": "hips", "keys": [[0, [0, 0, 0, 1]], [0.5, [0.02, 0, 0, 0.9998]]]},
    {"channel": "humanoid", "bone": "left_upper_arm", "keys": [[0, {"angles": [12, 5, 0]}], [1, {"angles": [80, 0, 10]}]]},
    {"channel": "humanoid", "bone": "left_index_proximal", "keys": [[0, [0.2588, 0, 0, 0.9659]]]},
    {"channel": "humanoid_position", "bone": "hips", "keys": [[0, [0, -0.02, 0]], [0.5, [0, 0.01, 0.15]]]},
    {"channel": "humanoid_position", "bone": "left_foot", "keys": [[0, [0.09, -0.9, 0.1]]]},
    {"path": "Hero/Hat", "channel": "rotation", "keys": [[0, [0, 0, 0, 1]]]}
  ]
}
```

**Tracks:**
- `humanoid`:
  - `bone` is one of the 22 slot names (`HumanBones.h`, e.g. `left_upper_arm`) or one of the 33
    extra names (6.3).
  - Values are quaternions `[x, y, z, w]`. A slot takes `E`; an extra bone takes its canonical rest
    delta.
  - Slots also accept `{"angles": [dof1, dof2, twist]}` in degrees. These are the pose system's DOFs,
    converted with `pose::jointRotation`, and written back as quaternions.
  - Eases and tangents work as on `rotation` tracks.
- `humanoid_position`:
  - `bone` is `hips` (the body offset, decision 3), `left_foot` or `right_foot`.
  - The two foot bones are the IK goals: the ankle's model-space position, expressed exactly like the
    hips offset (canonical, relative to the neutral hips, divided by humanScale).
  - Values are `[x, y, z]`, meaning (left, up, forward).
- `path` is absent on humanoid tracks; it is refused with a warning. Humanoid tracks are identified
  by `(channel, bone)`. They sort after every path track, by channel (in enum order), then by slot
  order, then by extra order.
- New channels are appended to `ClipChannel` (ClipTypes.h:30, which may only grow): `Humanoid` and
  `HumanoidPosition`.

**Top level:** `"humanoid"` (optional, informational: `source_model` and `human_scale` of the source).
The loader uses them only to show the scale ratio in reports.

**Versioning:**
- `ANIM_VERSION` becomes 2 (AnimationClipIO.h:49).
- `writeClip` writes `"version": 1` when the clip has no humanoid track, so v1 files round-trip
  byte-identically. It writes 2 otherwise.
- `parseClip` accepts 1 and 2. Humanoid channels in a version 1 file are kept, with a warning that
  the clip will be saved as version 2.

### 6.2 `.humanoid` sidecar version 2 (`engine/core/pose/HumanoidMapIO.{h,cpp}`)

```json
{
  "format": "jasperengine.humanoid",
  "version": 2,
  "animation_type": "humanoid",
  "bones": {"left_upper_arm": "upper_arm.L"},
  "extra_bones": {"left_index_proximal": "f_index.01.L", "jaw": ""},
  "twist": {"left_lower_arm": 15, "right_lower_arm": -15}
}
```

- `animation_type`: `generic` (the default) or `humanoid`. A humanoid model serves its model clips as
  humanoid clips.
- `extra_bones`: overrides with the same meaning as `bones`. A name forces the mapping, `""` forces
  the bone unmapped, and an absent key means auto-detect.
- `twist`: per slot, in degrees from -180 to 180. It is applied in `PoseRig::build` as
  `neutral_b ← neutral_b * axisAngle(neutralDirection_b, twist)`, in the bone's canonical neutral
  frame. It affects the neutral pose, so it applies to pose animations and to humanoid clips alike.
- `HumanoidOverrides` gets three fields: `std::optional<AnimationType> animationType`,
  `std::array<std::optional<std::string>, HUMAN_EXTRA_BONE_COUNT> extraBones` and
  `std::array<std::optional<float>, HUMAN_BONE_COUNT> twist`.
- `HUMANOID_VERSION` becomes 2. The writer writes version 1 when only `bones` is set, keeping the
  old byte form.

### 6.3 Extra bones (`engine/core/pose/HumanExtraBones.{h,cpp}`, new)

The `HumanExtraBone` enum lists these, in this order (a contract; append only):
- for each side (`left`, `right`), for each finger (`thumb`, `index`, `middle`, `ring`, `little`):
  `_proximal`, `_intermediate`, `_distal`. That is 30 bones, for example `left_thumb_proximal`.
- then `left_eye`, `right_eye`, `jaw`.

`autoDetectExtraBones(const Skeleton&, const HumanoidMap&, std::vector<std::string>* notes)` works as
follows:
- **Fingers.**
  - Candidate bones are the descendants of the mapped hand. Their `humanoidNameTokens` must contain
    the finger token. Synonyms: `pinky` maps to `little`. Thumb names: `thumb`, `thb`. Index names:
    `index`, `pointer`.
  - Candidates are ordered by depth along their chain. The first three are proximal, intermediate
    and distal.
  - Tip bones are skipped: their tokens include `end`, `nub`, `tip`, or a trailing ordinal `4`.
  - Mixamo `LeftHandThumb1/2/3` maps to proximal, intermediate and distal, as Unity maps it.
- **Eyes.** Bones under the head whose tokens contain `eye`, with a side token.
- **Jaw.** A bone under the head whose tokens contain `jaw`.
- **Ambiguity.** A bone nearer the root wins. Everything is reported in `notes`.
- `validateExtraBones` checks that each mapped name exists once, that no bone is also a slot bone,
  and that fingers descend from their side's hand.

`pose::resolveHumanoid` (ModelRig.h:58) also resolves the extra map. `ModelRigInfo` gets
`HumanExtraMap extra`.

## 7. Design: runtime C++

### 7.1 Data structures

- `ClipTrackData` / `AnimationClip::Track` get `int humanBone = -1`:
  - 0..21 is a slot;
  - 22..54 is an extra bone (`HUMAN_BONE_COUNT + extra index`);
  - for `humanoid_position`: 0 is hips, 1 is the left foot, 2 is the right foot.
- `AnimationClip::isHumanoid()` returns true when the clip has any humanoid track.
- `AnimationPlayerClip` (AnimationPlayerComponent.h) gets three flags, each written only when true,
  and each part of `operator==`:
  - `bool retarget = false`: play a generic clip as humanoid, converted from its source model;
  - `bool mirror = false`;
  - `bool footIK = false`.
- New `engine/core/animation/HumanoidClip.{h,cpp}`. It is pure and has no scene dependency:

```cpp
namespace jasperengine::anim{
    // Source description of a generic clip: its source rig and how its paths map to that rig's bones
    struct HumanoidSource{
        std::shared_ptr<const pose::PoseRig> rig;      // built from the source skeleton
        pose::HumanExtraMap extra;
        std::vector<Matrix4> bridges;                  // skeleton local = bridge * Transform local
        std::vector<std::string> bonePaths;            // per skeleton bone, its path from the model root
    };
    // Generic clip -> humanoid clip (decision 1). Samples the source on the union of its key times
    // and a frame-rate grid (clip frame_rate, at least 30 Hz), keeps step segments, writes one key
    // per sample per slot / extra / hips / foot goal, then keyframe-reduces each track (drops keys
    // whose linear/slerp reconstruction stays within 0.05 deg / 1e-4 humanScale). Non-humanoid
    // tracks (props, unmapped bones, morph, lattice, property) are copied unchanged. Events copied.
    JASPERENGINE_API bool convertToHumanoid(const ClipData& generic, const HumanoidSource& source,
                                            ClipData& out, std::vector<ClipWarning>& warnings, std::string* error);

    // Realises one humanoid sample on a target rig: local skeleton rotations of every present slot
    // and extra bone, the hips local position, optionally mirrored and with foot IK
    struct HumanoidSample{ std::array<Quaternion, pose::HUMAN_BONE_COUNT> E; std::bitset<pose::HUMAN_BONE_COUNT> hasE;
                           std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extra; std::bitset<…> hasExtra;
                           Vector3 hips; bool hasHips; Vector3 feet[2]; bool hasFeet[2]; };
    struct HumanoidTarget{ std::shared_ptr<const pose::PoseRig> rig; pose::HumanExtraMap extra;
                           std::array<int, pose::HUMAN_BONE_COUNT> effectiveSource; /* 7.3 chain tables */ … };
    JASPERENGINE_API void realiseHumanoid(const HumanoidTarget& target, const HumanoidSample& sample,
                                          bool mirror, bool footIK, pose::HumanPose& out,
                                          std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT>& extraLocal);
}
```

- **PoseRig additions** (PoseRig.h). These are public wrappers of existing internals:
  - `Quatd jointRotationFromLocal(HumanBone, const Quatd& local) const`, which is today's private
    `boneE` (PoseRig.cpp:344);
  - `Quaternion localFromJointRotation(HumanBone, const Quatd& E) const`, which is `left * E * neutral`;
  - `Quatd restWorldCanonical(int skeletonBone) const`, the canonical world orientation of any
    skeleton bone at rest, used for extra bones;
  - the `twist` corrections from the sidecar, applied inside `build` (6.2).
- **`pose::buildSkeletonFromGltf(const tinygltf::Model&, Skeleton&, std::vector<std::string>& bonePaths, std::vector<Matrix4>* bridges)`**
  is new, in ModelRig.cpp.
  - The bones are the union of all skins' joints, in glTF node order.
  - Names, parents restricted to bones, rest TRS, and the folding of non-bone ancestors into bridges
    all follow exactly what `buildSkeleton(scene, …)` derives from a loaded model.
  - An equality test pins it against the scene version (test 1).

### 7.2 Conversion (source to humanoid)

For each sample time `t` of the source clip:

1. For each skeleton bone with a rotation or position track, compute the skeleton local
   `L = bridge * TransformLocal(t)`. Bones without tracks use their rest.
   - **Path to bone matching.** First the bone's exact path from the model root. Otherwise the clip
     path minus its longest common prefix with the root model's node paths, which covers a clip made
     on a parent of the model. Otherwise the unique bone name, which is the last path segment.
   - A `relative` track is applied to the rest, as the player does.
2. Build the source `HumanPose`: slot locals and the hips local position.
   - For each mapped slot `b`: `E_b = rig.jointRotationFromLocal(b, L_b)`.
   - Hips: `offset = C^-1 (hipsWorldPosition(pose) - neutralHips) / humanScale`.
   - Feet: FK (`computeWorld`), then each foot's position as for the hips.
3. For each mapped extra bone `x` with skeleton parent chain: `δ = r_x^-1 * L_x`, where `r_x` is the
   rest local. Then `d_x = W_x δ W_x^-1`, where `W_x = rig.restWorldCanonical(x)`.
4. Hemisphere continuity per track. Then keyframe reduction, and the output is normalized
   (AnimationClipIO normalize).

- The output is not lossy for the source rig: playing it back on the source rig reproduces the
  source local rotations of every mapped slot and extra bone exactly, up to float precision and the
  reduction tolerance (test 1). Bones between slots that are not mapped keep their rest (they were
  not animated by humanoid playback). A warning lists animated source bones that are neither mapped
  nor kept as generic tracks.

### 7.3 Playback (humanoid to target) and the missing-slot rule

- With the rig's neutral-relative world frames `G_b = W_b * N_b^-1`, PoseRig gives
  `W_b = W_p * N_p^-1 * E_b * N_b` (the comment at PoseRig.cpp:441). So `G_b = G_p * E_b`, and `E_b`
  is the relative rotation between consecutive slots' `G`. Both are rig-independent.
- **Chains at bind time.** For each target slot `b` with effective target parent slot `p_t`, the bind
  step computes the chain of clip slots strictly after `p_t` and up to `b`, in hierarchy order
  (`HumanBoneInfo::parent`). Then per frame:
  `E_t(b) = Π_{s in chain} E_clip(s)`, from parent to child. A slot missing from the clip contributes
  `I`.
- **Local rotations.** The target skeleton local is `local_b = rig_t.localFromJointRotation(b, E_t(b))`.
  It is written to the bone entity as `Transform.rotation = bridgeRotationInverse_b * local_b`, the
  same bridge rule as PoseAnimatorRuntime.h:134-136.
- **Hips position.** `hipsModel = neutralHips_t + C_t * (offset * humanScale_t)`. It goes through
  `rig_t.setHipsWorldPosition`, then the hips bridge inverse, then onto `Transform.position`.
- **Extra bones.** `local_x = r_x^t * (W_x^t)^-1 * d_x * W_x^t`.
- **Mirror** (entry flag):
  - Left and right slots swap, and so do the extra bones and the foot goals.
  - Each quaternion `(w, x, y, z)` becomes `(w, x, -y, -z)`, because x is the canonical left axis.
    This is the same mirror as `engine/tests/pose/TestRigs.h:86`.
  - Positions get their x negated.
  - Mirroring twice gives back the identity.
- **Foot IK** (entry flag, when the clip has foot goals). After the steps above, the target
  `HumanPose` is complete. Then:
  `pose::solveLimbIK(rig_t, pose, LeftLeg, goalModel, keepEndWorldRotation = true)`, and the same for
  the right leg, with `goalModel = neutralHips_t + C_t * (goal * humanScale_t)`.
  - An unreachable goal clamps, which is PoseIK's existing rule.
- **Contributions.** The resulting slot rotations, the hips position and the extra rotations are
  contributions to the state's bindings, through the normal accumulators. Rotation goes to the bone
  entity's Rotation channel and the hips to its Position channel, both with the state's weight. So
  crossfades, layers, masks (body parts) and the rest fill (R1, R2) apply unchanged.

### 7.4 The player and the pool

- **AnimationClipPool** (`engine/core/pool/AnimationClipPool.{h,cpp}`):
  - A model sub-clip whose model's sidecar says `animation_type: humanoid` is converted after
    `buildModelSubClip`. Its cache key is unchanged. It is invalidated with the model or with a
    sidecar edit: the editor's sidecar writer calls `AnimationClipPool::invalidateModel`.
  - A new key form, `"<clip reference>@humanoid"`, loads the clip and converts it from its source
    model:
    - for a sub-clip, the model;
    - for a `.anim`, `source.path` when `source.kind == "model"`, otherwise `preview.model`.
    - If neither exists, the error is "retarget: the clip names no source model (set its Preview
      Model)".
  - A clip that is already humanoid is returned as is.
  - Source rigs are cached by `(model path, sidecar stamp)` in a `HumanoidSourceCache` beside
    `PoseRigCache`.
- **Binding.** `PlayerStateRuntime` builds its pool key from the entry: `clip + (retarget ? "@humanoid" : "")`.
  When `resolveBindings` (ActionSystemPlayers.cpp:113) sees humanoid tracks:
  - It resolves the target rig once per player: `AnimationPlayerRuntime::humanoid`, which holds the
    `ModelRigInfo` and the bone entities. It is rebuilt when the models are reloaded (R10) or when
    the sidecar stamp changes.
  - Each humanoid track gets as its target the slot's or extra bone's entity. When the target lacks
    the bone, the track is unresolved but is still used for chains (7.3).
  - `ChannelRest` is taken for those entities as usual.
- **Sampling.** `accumulateClip` (:430) treats a humanoid clip in three steps:
  1. it samples every humanoid track into a `HumanoidSample`, using the same key cursors;
  2. it calls `realiseHumanoid`;
  3. it contributes Rotation and Position on the target entities.

  Generic tracks of the same clip go through the existing path. The layered evaluator (#10) receives
  the same per-entity contributions.
- **Preview.** Edit-mode preview (`previewPlayer`, :971) supports humanoid clips the same way, with
  its own rig lookup per session.
- **PoseAnimator.** It is unaffected, except for twist corrections (6.2).

### 7.5 Additive humanoid layers

An additive layer (#10) with a humanoid clip computes its delta per target bone, from the realised
local rotations at `additive_reference`. This is the generic rule, applied after realisation.

### 7.6 Generic tracks inside humanoid clips

A generic path track of a humanoid clip that does not resolve by path is tried again by unique bone
name under the target model, using the last segment. Examples are props, unmapped bones and twist
bones. This lets Mixamo characters whose glTF root nodes are named differently (`Hero/…` versus
`Granjero/…`) share non-humanoid bones. Such a track writes absolute values, as today. The binding
report marks `bound_by_name`.

## 8. Lua API

- `AnimationPlayer:addClip(path [, name [, options]])`. `options` is a table:
  `{retarget = bool, mirror = bool, footIK = bool, layer = int}`. `layer` comes from #10.
- `AnimationState`:
  - `mirror` (rw) and `footIK` (rw) are runtime overrides of the entry's flags.
  - `isHumanoid` (ro) is true for a humanoid clip, or for a generic one being retargeted.
- `Model:getHumanoidInfo()` returns a table `{humanoid = bool, animationType = "generic"|"humanoid",
  humanScale = number, missing = {slot names}, extraMapped = int}`. It returns `nil` when the model
  has no bones.
- There are no Lua conversion APIs. Conversion is an asset operation: the editor, MCP, or the pool
  at load.
- The C++ `AnimationPlayer` / `AnimationState` get the same members. The stubs are regenerated.

## 9. Editor UX

- **Model Inspector**, Model section, a new **Rig** foldout:
  - **Animation Type**: Generic / Humanoid. Changing it writes the sidecar as one project undo step
    and invalidates the model's clips.
  - A status chip: `Humanoid 22/22 · fingers 28/30 · eyes 2 · jaw` or `Missing: left_hand`. It uses
    the Pose window's chip colours.
  - **Configure Avatar...**.
  - **Human scale** (read-only).
- **Avatar Configuration window** (`editor/window/AvatarWindow.{h,cpp}`, opened from the Rig
  foldout, the Pose window's Map Bones..., or a player's Bindings report):
  - **Mapping tab.**
    - Two tables, **Body** (22 slots) and **Hands & Head** (33 extra bones).
    - Each row has a combo of skeleton bones with search, an **Auto** entry (back to auto-detect) and
      **None** (forced unmapped).
    - Slots that are required but missing are red. Auto-detected rows are grey and overrides are
      white.
    - Buttons: **Auto Map All**, **Clear Overrides**.
  - **Pose tab.**
    - Shows the model in the Scene view in its **neutral pose** (the rig's computed neutral, the
      reference every humanoid clip is relative to).
    - **Twist corrections**: a slider per limb slot, -180 to 180, with live preview.
    - **Test clip** field: any clip. It is played retargeted with a scrubber, using the edit-mode
      preview.
    - Toggles: **Mirror** and **Foot IK**, for testing.
  - **Notes**: the auto-detect notes.
  - Edits apply to the preview live. **Apply** writes the sidecar as one project undo step; it is
    the same writer as the existing `set_humanoid`. Closing with changes asks Apply, Discard or
    Cancel.
- **Animation Player entries** (InspectorWindowClips.cpp):
  - A generic clip gets a **Retarget** checkbox. Its tooltip says "Play as a humanoid clip,
    retargeted from its source model".
  - A humanoid or retargeted clip gets **Mirror** and **Foot IK** checkboxes and a **Humanoid**
    badge.
  - The Bindings report lists, per humanoid state:
    - the target rig (model name and human scale);
    - the scale ratio to the source;
    - the target slots fed by a chain fold (e.g. "chest folded into upper_chest");
    - the clip bones the target lacks.
- **Convert to Humanoid Clip...** is in the context menu of a `.anim` or model clip in the Project
  window, the clip asset Inspector, and the player entry's clip field menu.
  - Its dialog has: **Source model** (default: the source or preview model; required), **Output**
    (default `<name> (Humanoid).anim` next to the source), and **Use in this player** (switch the
    entry; one scene step).
  - Otherwise the conversion is one project step that writes the new file.
  - **Extract to .anim** of a humanoid model clip writes the humanoid clip.
- **Animation window with a humanoid clip:**
  - Rows read `Humanoid : Left Upper Arm`. Their foldout shows `raise`, `forward` and `twist` in
    degrees, and the curves view draws these three curves.
  - Hips position rows are `Humanoid : Body Position` (left, up, forward) and
    `Humanoid : Left Foot Goal`.
  - **Add Property** gets a **Humanoid** branch with every slot and extra bone, plus body position
    and foot goals.
  - **Record.**
    - It works when the bound root has a humanoid rig. Rotating a mapped bone keys its slot's `E`,
      computed with the bound rig. Moving the hips keys the body position.
    - Unmapped bones key generic tracks.
    - Without a humanoid rig, Record on a humanoid clip is disabled. The status line says "bind a
      humanoid character to record this humanoid clip".
- **Pose Animation window**, Bake menu: **Bake to Humanoid Clip** writes `E` for every slot directly
  from the pose frames. The hips offset and foot goals come from the realised frames on the preview
  rig. Like the existing Bake to Animation Clip, it is one scene step: the file plus a player entry.

## 10. MCP and agent surfaces

- **`edit_animation`.**
  - `create: {from: "<clip>", humanoid: {model?: "<source model>"}}` converts the clip to a humanoid
    file. It refuses when no source model is known.
  - Tracks may be `{channel: "humanoid", bone, keys}` with key values `{quat}` or `{angles}`, or
    `{channel: "humanoid_position", bone: "hips"|"left_foot"|"right_foot", keys}`. They are refused
    with `path`. An unknown bone gets a did-you-mean.
- **`get_animation`.**
  - `{clip}` lists humanoid tracks with `bone`. Samples give `{quat, angles}` per key.
  - `{clip, root, times}` on a humanoid clip also returns `realised: [{bone, entity, path, local_rotation: {quat, euler}, angles}]`
    and `hips_local_position` at each time, on the root's rig. Agents check retargets numerically
    with it.
  - `retarget_report: {target: {model, human_scale}, source: {model, human_scale}, scale_ratio,
    folded: [...], missing_on_target: [...], unmapped_source_bones: [...]}`.
- **`set_properties`**: `animationplayer.clips[i].retarget|mirror|footIK`.
- **`edit_pose_animation` op `set_humanoid`** (existing) gets the fields `animation_type`,
  `extra_bones` and `twist`. It also works without a pose animation `path`, for avatar-only edits.
- **`get_asset_info`** on a model gains
  `avatar: {animation_type, humanoid, mapped, missing, extra_mapped, twist, human_scale, notes}`.
- **Guides:**
  - a "Humanoid clips and retargeting" section in `docs/mcp/guide/animation.md` (about 50 lines);
  - the pose-animation guide's `set_humanoid` row;
  - the binding report's `rest_mismatch` `rig_mismatch` text now suggests `retarget: true` or a
    Convert, instead of "give that character its own clip".
- **Budget**: tool definitions grow by at most 900 characters in total.

## 11. Exported-build path

- Everything runs in the engine: `HumanoidClip`, the pool conversion, PoseRig and the glTF skeleton
  builder.
- `.humanoid` sidecars ship with the assets. `Exporter::copyAssets` copies them already, as PoseAnimator
  needs them.
- An FBX source is shipped as its `.glb` import, which the sub-clip builder reads.
- New component flags go through `editor/Stream.cpp` (AnimationPlayer encode/decode at 7365 and 5343)
  and `editor/Factory.cpp:1916`.
- No per-platform code.

## 12. Behaviour rules and edge cases

- **H1. Identity on the source rig.** A humanoid clip on its own source rig reproduces the source's
  mapped bones (test 1). Generic tracks are copied, so a converted clip plays like the original on
  the source, except for unmapped intermediate bones (warned).
- **H2. Several models under the root.** The first humanoid model in hierarchy order is the target.
  The binding report names it. A second humanoid character needs its own player.
- **H3. Sidecar edited during play.** The rig is rebuilt on the next update (stamp change), and the
  humanoid states rebind. A source sidecar change invalidates converted clips (pool generation), and
  players reload them (R12-style pool generation check).
- **H4. Mixing humanoid and generic clips on the same bones.** They blend normally. A generic clip
  authored for another rig still looks wrong, as today. The binding report's `rest_mismatch` now
  suggests Retarget.
- **H5. A non-humanoid target.** Humanoid tracks are unresolved, with one warning per player and
  clip. Generic tracks play.
- **H6. Scale.** `humanScale` is the neutral hips height / 0.95 (PoseRig.h:64). Non-uniform model
  scale is not compensated. A scaled Model entity scales the result through its Transform, as with
  any clip.
- **H7. Mirror on asymmetric rigs.** A missing mirror counterpart slot keeps the neutral pose.
- **H8. Twist corrections and existing pose animations.** Corrections change pose animations on that
  model too. The Avatar window says so in its Pose tab.
- **H9. Async conversion.** While a retargeted clip converts, the state waits (calls are kept), as
  for model sub-clips (`pollOrLoad`). A failed conversion is an error once, retried like a load
  failure (1 s).
- **H10. Determinism.** Conversion is deterministic for the same input: the same file bytes from
  Convert, which keeps diffs readable.

## 13. Performance (mobile)

- **Playback.**
  - Per humanoid state per frame: up to 22 slot realisations (2 quaternion products plus the chain
    products), up to 33 extra-bone conjugations, and a hips transform.
  - Foot IK adds one FK pass over 22 slots and two analytic two-bone solves.
  - Budget on a mid-range Android phone: 30 humanoid characters with one state each and Foot IK on
    must cost ≤ 0.8 ms per frame on top of generic playback. A host test asserts ≤ 0.3 ms in a
    Release build.
  - No allocation per frame: `HumanoidSample` and the chains are preallocated per binding.
- **Conversion.** One sample per frame at ≥ 30 Hz. A 3 s clip on a 65-bone rig must convert in
  ≤ 5 ms on the host. The conversion is cached per clip and done once per load.

## 14. Tests

Engine pose and anim tests (`engine/tests/pose` `jasperengine-pose-tests`, `engine/tests/anim`
`jasperengine-anim-tests`) use `TestRigs.h` (`tposeRig`, `aposeRig`, `backwardRig`, `skewedRig`,
`minimalRig`). Model tests use `tools/fixtures/pose-animation/assets/models/{hero_animated,hero_tpose,robot_apose}.gltf`.

1. **The glTF skeleton.** `buildSkeletonFromGltf(hero_animated)` equals `buildSkeleton(scene, hero)`:
   names, parents, rest TRS within 1e-6, and bridges.
2. **Round trip on the source rig.** Convert `hero_animated.gltf#Walk` and play it on hero.
   - At t ∈ {0, 0.25, 0.5, 0.75}, every mapped slot's local rotation matches the source within
     0.05° (with reduction) or 1e-4° (without).
   - The hips local position matches within 1e-4·humanScale.
   - The mapped fingers' local rotations match within 0.05°.
3. **Cross-rig angles.**
   - Source `tposeRig` pose: `left_upper_arm.raise = 60`, `left_lower_arm.bend = 30`,
     `spine.lean = 10`. Convert to humanoid and realise on `aposeRig`.
   - `anglesFromLocal` on the target gives the same DOFs within 0.05°.
   - The left forearm's canonical world direction matches the source's within 0.1°.
   - The same holds on `backwardRig` (a rig facing backwards) and on `skewedRig`.
4. **Hips scaling.** The target is `tposeRig` scaled ×0.5 (humanScale halves). A source offset of
   (0, 0.05, 0.3) humanScale units gives a target hips displacement of exactly half the source's, in
   model space, along the target's canonical axes (1e-5).
5. **Missing slot fold.** The source has a chest `E` of 20° about the canonical left axis. On
   `minimalRig` (no chest, no upper chest), the target's spine or neck chain gets a canonical world
   orientation `G` for its next present slot equal to the source's `G` for that slot (0.01°).
6. **Mirror.** A clip raising the left arm, mirrored, raises the right arm with the mirrored angles:
   twist negated as `mirrorBodyPose` does (0.05°). Applying the mirror twice is the identity.
7. **Foot IK.** On `skewedRig` (legs ×1.2), with a walk converted from `tposeRig`, both ankle
   positions reach `goalModel` within 1e-3·humanScale on every sampled frame where the goal is
   within reach. Foot world rotations are unchanged by the solve.
8. **Extra bones across axis conventions.** The target finger bones' rest local frames are rotated
   90° about the bone axis versus the source. A 30° curl on the source index proximal gives an equal
   canonical-world delta on the target (0.1°).
9. **Twist correction.** A sidecar `twist.left_lower_arm = 90` rotates the neutral forearm by 90°
   about its neutral direction. A humanoid clip with `E = I` shows that rotation. A `.poseanim`
   neutral pose on the same model shows it too.
10. **IO.**
    - `.anim` v1 files round-trip byte-identically.
    - A clip with humanoid tracks writes `version: 2`.
    - `{angles}` keys are written back as quaternions.
    - A humanoid track with a `path` is refused, with its JSON path.
    - Sidecar v1 round-trips byte-identically. v2 fields parse, and unknown extra names warn.
11. **Pool.**
    - `hero_animated.gltf#Walk` with a humanoid sidecar is served as humanoid (`isHumanoid`).
    - `robot_apose` plays it: `left_upper_leg` angles at t = 0.5 match hero's within 0.1°, and there
      is no NaN.
    - `"<anim>@humanoid"` without a source model fails with the documented error.
12. **Layers interplay** (#10). A generic base walk, plus a humanoid wave on layer 1 with a
    `right_arm` humanoid mask. The right forearm equals the realised wave (0.05°). The left forearm
    equals the base walk.
13. **Editor tests.**
    - The Avatar window driver: map a slot, set a twist, Apply writes the sidecar. Undo restores the
      bytes.
    - Convert writes the expected file. Record on a humanoid clip keys `E` (it equals
      `jointRotationFromLocal` of the gizmo's local rotation).
    - Stream round trip of the entry flags.
14. **MCP test** (`tools/mcp/tests/test_animation_retarget.py`, `pose-animation` fixture):
    - set Robot's player entry to `hero_animated.gltf#Walk` with `retarget: true`;
    - `get_animation {clip: "assets/models/hero_animated.gltf#Walk", root: "Robot", times: [0.5]}`
      (the entry's `retarget` flag makes it report the humanoid realisation). Check that `realised`
      has the 22 slots, with `scale_ratio` between 0.5 and 2;
    - play, then `get_runtime_entity` on `thigh.L`: its rotation is not the rest rotation and
      matches `realised` (1e-3);
    - `edit_animation create {from, humanoid}` writes a v2 file;
    - check the tool budget.

## 15. Acceptance criteria

- [ ] Tests 1–14 pass. All existing pose and animation tests pass unchanged. Old `.anim` and
  `.humanoid` files keep byte-identical round trips.
- [ ] In the editor and in an exported Linux build, `hero_animated.gltf#Walk` plays on
  `robot_apose.gltf`:
  - the arms go down, not to a T;
  - the hips are at the robot's height;
  - the feet are on the floor, and with Foot IK they do not slide visibly (feet within 1 cm/m of the
    goals).
- [ ] An Animation Type: Humanoid model serves its clips retargeted to any humanoid player, with no
  per-clip step.
- [ ] The Avatar window maps bones, previews the neutral pose and a test clip, applies twist, and
  writes the sidecar with undo.
- [ ] The Animation window edits a humanoid clip as angle curves and records it on a humanoid
  character.
- [ ] Docs:
  - `docs/animation-clips.md`: a new "Humanoid clips and retargeting" section, and the "Sharing a
    clip: Another character rig" paragraph is rewritten to point to it;
  - `docs/pose-animation.md`: twist corrections, extra bones and the Avatar window;
  - `docs/unity-glossary.md`: Avatar / Humanoid Animation Type / muscle clip / Mirror / Foot IK rows;
  - `docs/mcp/guide/animation.md` and `pose-animation.md`;
  - `lua-api.md`: the new members;
  - the README feature list gets "humanoid retargeting".
- [ ] The performance budget of section 13 is met.

## 16. Integration points

- `engine/core/animation/ClipTypes.h:30`: append `Humanoid` and `HumanoidPosition` to `ClipChannel`.
- `engine/core/animation/AnimationClip.h:80-90, 150-170`: `humanBone`, `isHumanoid`.
  `AnimationClipIO.cpp`: `channelNames` (:112), track parsing (:975-1002), the writer (:1285), the
  version (:1387-1398, `ANIM_VERSION` AnimationClipIO.h:49).
- `engine/core/pool/AnimationClipPool.{h,cpp}`: the humanoid model serve, the `@humanoid` keys, and
  invalidation on sidecar change.
- `engine/core/animation/ModelSubClips.cpp`: `buildModelSubClip` hands its tinygltf model to
  `buildSkeletonFromGltf` for conversion.
- `engine/core/pose/PoseRig.{h,cpp}`: public `jointRotationFromLocal` / `localFromJointRotation` /
  `restWorldCanonical`, and twist in `build` (:166-204).
- `engine/core/pose/HumanoidMapIO.{h,cpp}`: v2 fields; :66-69 known keys.
- `engine/core/pose/ModelRig.{h,cpp}`: `buildSkeletonFromGltf`, and the extra map in
  `resolveHumanoid` / `ModelRigInfo`.
- New: `engine/core/pose/HumanExtraBones.{h,cpp}`, `engine/core/animation/HumanoidClip.{h,cpp}`.
- `engine/core/subsystem/ActionSystemPlayers.cpp`: `resolveBindings` :113, `accumulateClip` :430,
  `preparePlayer` :567 (rig wait), `previewPlayer` :971.
- `engine/core/component/AnimationPlayerComponent.h:38-48`: entry flags.
- `engine/core/script/binding/GameObjectClassesLua.cpp:1809-1865` (AnimationState, addClip options),
  and the Model binding for `getHumanoidInfo`.
- Editor:
  - `editor/window/AvatarWindow.{h,cpp}` (new);
  - `editor/window/InspectorWindow.cpp` (Model section, Rig foldout);
  - `InspectorWindowClips.cpp`, `AnimationWindowClip*.cpp`;
  - `editor/animation/ClipConverters.cpp` (Convert to Humanoid);
  - `ClipRecordHooks.cpp` (humanoid record);
  - `editor/pose/PoseBaker.{h,cpp}` (Bake to Humanoid Clip);
  - `editor/mcp/tools/AnimationClipTools.cpp`, `PoseTools.cpp` (`set_humanoid`), `AssetTools.cpp`
    (`get_asset_info` avatar);
  - `editor/Stream.cpp`, `editor/Factory.cpp`.

## 17. Cross-feature dependencies

- **`anim-layers` (#10), required first.**
  - Humanoid contributions enter the layered evaluator.
  - Humanoid body-part masks share this feature's target-rig lookup. `anim-layers` defines the lookup
    rule, and this feature caches it in `AnimationPlayerRuntime::humanoid`.
  - Controller states' `mirror`, `foot_ik` and `retarget` flags (#10 phase 2) map to the entry flags
    defined here.
- **`anim-properties` (#12)** comes before this feature in the merge order. Property tracks in
  converted clips are copied unchanged. Both features append to `ClipChannel`; property is already
  present, so there is no conflict. The `.anim` version bump to 2 is owned by this feature alone.
- **`physics-3d` (#9) ragdoll** is independent. A disabled player hands bones over to physics.

## Implementation notes

Decisions taken where the spec was silent or ambiguous, and deviations. Code references are to
the `gaps/anim-retarget` branch (based on `gaps/anim-layers`).

### Format and maths

- **Conversion is key for key, not resampled** (7.2). A slot's `E = left^-1 * bridge * q * N^-1`
  and an extra bone's `d = W * rest^-1 * bridge * q * W^-1` are fixed linear maps of the source
  quaternion `q`, which commute with slerp, Hermite curves, eases and normalization; the body
  offset is an affine map of the hips translation. So every slot, extra and hips track keeps the
  source's key times, eases and (mapped) tangents and reproduces the source exactly on its own rig
  (no keyframe reduction is needed there). Only the foot goals depend on several tracks: they are
  sampled with FK on the union of the leg tracks' key times and a frame grid (clip rate, at least
  30 Hz) and reduced within 1e-4 humanScale. A mapped slot without a track gets one key from its
  rest; the hips position always gets a track (one key without a translation track).
- **Dropped source tracks**: rotation or translation tracks of non-slot bones *between* slots (an
  ancestor of a slot bone, e.g. twist bones, an unmapped spine bone, a root bone above the hips)
  and translations of mapped bones other than the hips are dropped with one warning listing them;
  scale tracks and every other bone (props, leaf bones, unmapped fingers) are copied unchanged.
- **Extra bones use the neutral pose's frame** (7.2 step 3, 7.3): `W_x` is the bone's canonical
  world rotation in the rig's *neutral* pose (FK of the neutral slots, the fingers at rest), not in
  its rest pose. With the rest frame a T-pose source's finger curl would land on an A-pose target
  about a world axis that is not its finger's; with the neutral frame both hands point the same way
  first. `PoseRig::restWorldCanonical` exists as specified (and is tested through the rig), the
  realisation uses the neutral frame.
- **Twist corrections** rotate `N_b` about the slot's neutral direction in the canonical *world*
  frame (`N_b <- axisAngle(dir, t) * N_b`, which keeps the direction; the spec's
  `neutral_b * axisAngle(...)` would rotate about a bone-local axis). Carried children (the hand
  under a twisted forearm) turn with it, so a forearm correction also fixes the wrist. Applied in
  `PoseRig::build` (a new optional argument), so pose animations (PoseRigCache and the editor's
  PoseRigProvider) and humanoid clips see the same neutral pose.
- **Slots the clip lacks are written neutral** (decision 4): a humanoid clip with slot tracks writes
  every present slot of the target (`E = I` when no clip slot is on its chain), as Unity's muscle
  clips write every muscle; use a mask (or a layer) to animate part of the body. The hips position
  is written only when the clip has a body offset track (the converter always writes one).
- **Version 2** is written only for clips with humanoid tracks; the `humanoid` top-level info is
  written when set. A humanoid track found with a `path` is dropped (the parser's warning names
  `tracks[i].path`); hand-built data with a path keeps the track with the path cleared (normalize).
  `ClipData::findTrack` / `AnimationClip::findTrack` with a humanoid channel take the bone name as
  the path: the editor's (path, channel) key identities work unchanged for humanoid rows.
- **Sidecar version 2**: `animation_type: generic` is never written (reset to absent), and a twist
  of 0 clears the entry, so going back to a bones-only Avatar gives the version 1 bytes again.
  An `extra_bones` override that names a missing bone, a slot's bone, or a finger outside its
  hand fails `resolveHumanoid` like a bad `bones` override; the editor's PoseRigProvider then
  falls back to auto-detect for the extra bones (the body map still poses) and reports
  `extra_error`.

### Runtime

- **Shared evaluator changes are additive**: humanoid tracks never get a path target; a
  `HumanoidBinding` on `ClipBindings` lists its outputs (bone entity, Rotation or Position), which
  `buildPoseSlots` registers as ordinary transform slots (`PoseSlotSource::humanoidSlots`), the
  mask stamp masks like any bone (`humanoidMask`), the occlusion pass covers, and one block per
  state in `composePlayerLayers` adds the realised values (additive: their delta from the
  realisation at `additive_reference`, `humanoidReference`). The v1 path (`accumulateClip`) adds
  the same contributions with rest flags. Property slots (anim-properties) are untouched.
- **Rig lookup**: `AnimationPlayerRuntime::humanoid` (HumanoidSlots, now with a `HumanoidTarget`:
  rig, bone entities, bridges, extra entities and frames, fold chains) is resolved by
  `refreshHumanoidRig` in `preparePlayer` when a humanoid clip, a retarget entry or a humanoid mask
  needs it; again after a model reload, when one of its bone entities is gone, and on a pool change
  (a sidecar edit invalidates the model's clips) when the rig changed. A state rebinds when
  `humanoidRevision` moved. `preparePlayerLayers` is now called with `modelsReloaded = false`
  (the reset happened in `preparePlayer`).
- **Pool**: `"<ref>@humanoid"` entries are cached under `"<entry key>@humanoid"`; `invalidate` of
  the clip drops its conversion, `invalidateModel` drops sub-clip conversions with the model and
  `.anim` conversions whose source is that model (`humanoidSources`), and the HumanoidSourceCache
  entry. A humanoid-typed model's sub-clip that fails to convert (not a valid humanoid) is served
  generic with a warning instead of failing. The source rig is built from a full parse of the model
  (the pooled glTF when loaded): the trimmed sub-clip parse has no skins or meshes, and the skin
  sample makes the source rig equal to the scene rig of the same model (exact round trip).
- **Mirror** of a blend tree child flips its state's (XOR). Controller states' `retarget`,
  `mirror` and `foot_ik` become the player states' flags; the Animator window's state inspector
  has the three checkboxes (a blend tree child's mirror is edited with `edit_animator`).
- **Lua** `addClip(path, name | nil, options)`: `name` may be nil in the three-argument form.

### Editor and MCP

- **Avatar Configuration window** (`editor/window/AvatarWindow.{h,cpp}`) is a floating window
  opened from Inspector > Model > Rig > Configure Avatar..., the Pose window's new **Avatar...**
  button beside Map Bones..., and the Bindings report's Map Bones... of a humanoid state on a
  root without a rig. **Auto Map All** puts every body and hand bone back to auto-detect;
  **Clear Overrides** also clears the twists (the Animation Type stays). The test clip is any clip
  played through `"@humanoid"` (a generic one converts from its source model) on the working
  copy's rig, so unsaved mapping and twist edits preview.
- **Convert to Humanoid Clip...** is one dialog (`HumanoidConvertDialog`) for the Project window's
  `.anim` menu, the clip asset Inspector (`.anim` and model clips, in the model asset view too) and
  a player entry's clip menu (**Use in this player**: `ClipSceneActions::convertToHumanoid`, one
  scene step with the file; the entry's `retarget` is cleared since the file is humanoid).
- **Map Bones... keeps the Avatar fields**: the Pose window's mapping popup now writes the bones
  into the sidecar as read (it used to write a bones-only sidecar, which would drop
  `animation_type`, `extra_bones` and `twist`).
- **Humanoid rows in the Animation window**: the value column shows the bound rig's current value
  while previewing (else the clip's), and editing a humanoid component keys it at the playhead
  (there is no GameObject value to preview in pink). Slots show joint angles, extra bones Euler
  degrees; twist unwraps by whole turns along the curves.
- **`get_animation` realised** lists every slot the target has (the robot fixture has no upper
  chest: 21 slots, the MCP test checks at least 21) and the mapped extra bones the clip animates;
  `realised` is `[{time, bones: [...], hips_local_position}]`.
- **`set_humanoid` without `path`** (only set_humanoid ops) writes the sidecars as one project step
  through ProjectFileWrites (their `filesChanged` invalidates the models' clips); the result has
  `sidecars` and `avatars`.
