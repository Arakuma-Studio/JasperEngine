# Pose Animation, phase 1: the pose core (humanoid rig, joint angles, tween, humanize, IK)

Status: approved design, ready for implementation.
Branch: `pose-animation`. Phases 2-6 build on the contracts defined here and must not redefine
them; a later phase may only extend them.

## 1. Goal

JasperEngine gets a pose-to-pose character animation tool: the user (or an AI agent) sets a few
key poses on a humanoid character, the engine snaps between them with springy, stylised
transitions, adds humanising secondary motion, and the result is either baked to a standard
Animation or played live at runtime. The idea comes from the Unity package "Vibrations"
(ValDesu/Vibrations). That repository has **no licence**: nothing from it is copied (code,
shaders, template values, images). Phase specs describe our own implementation of the ideas;
the standard maths (an underdamped spring step response, analytic two-bone IK, damped springs)
is textbook material.

The whole feature, by phase:

1. **The pose core** (this spec): pure C++ maths in `engine/core/pose/`, no ECS, no editor.
   The humanoid bone set and auto-detected mapping, the rig (FK, character frame, neutral pose,
   scale), the rig-independent joint-angle representation, mirroring, joint limits, the tween
   and feel model, humanize, follow-through, auto timing, two-bone IK, grounding, CPU skinning
   for the exact lowest point, and a googletest target.
2. **Asset, runtime, bake, agent tools**: the `.poseanim` file, the `.humanoid` sidecar, the
   runtime Pose Animator component and Lua API, baking to an Animation, the editor document and
   undo, MCP `get_pose_animation` / `edit_pose_animation`.
3. **Offscreen render and checks**: rendering a character in arbitrary poses to images without a
   window, contact sheets with labels and left/right colours, numeric pose metrics, MCP
   `render_pose_animation`.
4. **The Pose Animation window**: pose strip, timing bar, feel and humanize panels, preview, bake.
5. **Viewport posing**: IK handles, hips handle, joint picking, rotation rings, floor, onion skin,
   Global Edit.
6. **Templates and docs**: built-in rig-independent templates, user templates, the agent guide.

Decisions taken by the owner: bake **and** runtime playback; joint angle 0 = a **neutral
standing pose**; phases implemented in full.

## 2. Background

Research notes (scratch, outside the repo; read the relevant one before starting):

- `/tmp/pose-research/vibrations-animation-core.md`: the analysed algorithms (ideas only).
- `/tmp/pose-research/vibrations-ui-and-ai.md`: the analysed UX and AI tool design.
- `/tmp/pose-research/engine-animation-editor.md`: what the engine and editor have today, with
  file:line evidence (ActionSystem, keyframe tracks, bones, AnimationWindow, gizmos, previews).
- `/tmp/pose-research/engine-agent-surface.md`: the MCP server, its rules and the tool budget.

Key engine facts:

- Bones are ordinary entities with `Transform` and `BoneComponent`; `ModelComponent` holds
  `skeleton`, `bonesNameMapping`, `bonesIdMapping`, `animations`.
- Model space is Y-up (glTF). A glTF character faces +Z and its left is +X.
- `Quaternion` (`engine/core/math/Quaternion.h`) is (w, x, y, z), right-handed, with
  `Quaternion(angle, axis)` in the engine's angle unit (`Angle::defaultToRad`): **the pose core
  never relies on the default unit**: it works in radians internally through its own helpers
  (`pose/PoseMath.h`, section 4.7) and exposes degrees in its public angle API.
- googletest is available from `libs/yaml-cpp/test/gtest-1.11.0` (used by the HTML UI tests).

## 3. Concepts and naming (contract)

| Name | Meaning |
|---|---|
| Pose Animation | the source asset (`.poseanim`, phase 2): key poses + feel + humanize |
| Pose (key pose) | one entry of a Pose Animation: a body pose plus hold, transition, ground flags |
| Body pose (`BodyPose`) | rig-independent pose: joint angles in degrees + horizontal hips offset |
| Human pose (`HumanPose`) | rig-specific realised pose: local rotations of the 22 slot bones + hips local position |
| Humanoid map (`HumanoidMap`) | slot -> skeleton bone name, auto-detected, overridable (`.humanoid` sidecar, phase 2) |
| Rig (`PoseRig`) | a skeleton + humanoid map, with rest pose, character frame, neutral pose, scale |
| Pose Animator | the runtime component that plays a Pose Animation on a model (phase 2) |
| Feel | the transition settings (curve, snap time, overshoot, wind-up, choppiness, overlap, looseness) |
| Humanize | joint limits, moving holds, in-betweens, life, follow-through |

C++ namespace: `jasperengine::pose`. Files: `engine/core/pose/*.{h,cpp}`. Every class/struct
that phase 2+ uses from the editor is exported with `JASPERENGINE_API`.

## 4. Contracts

### 4.1 Human bones (`HumanBones.h`, written up front, do not change the order)

22 slots, fixed order and snake_case names:

| # | slot | name | parent slot | mirror | group | depth | required |
|---|---|---|---|---|---|---|---|
| 0 | Hips | `hips` | - | - | Spine | 0.0 | yes |
| 1 | Spine | `spine` | Hips | - | Spine | 0.2 | yes |
| 2 | Chest | `chest` | Spine | - | Spine | 0.3 | no |
| 3 | UpperChest | `upper_chest` | Chest | - | Spine | 0.4 | no |
| 4 | Neck | `neck` | UpperChest | - | Head | 0.55 | no |
| 5 | Head | `head` | Neck | - | Head | 0.7 | yes |
| 6 | LeftShoulder | `left_shoulder` | UpperChest | 10 | Arms | 0.45 | no |
| 7 | LeftUpperArm | `left_upper_arm` | LeftShoulder | 11 | Arms | 0.6 | yes |
| 8 | LeftLowerArm | `left_lower_arm` | LeftUpperArm | 12 | Arms | 0.8 | yes |
| 9 | LeftHand | `left_hand` | LeftLowerArm | 13 | Arms | 1.0 | yes |
| 10 | RightShoulder | `right_shoulder` | UpperChest | 6 | Arms | 0.45 | no |
| 11 | RightUpperArm | `right_upper_arm` | RightShoulder | 7 | Arms | 0.6 | yes |
| 12 | RightLowerArm | `right_lower_arm` | RightUpperArm | 8 | Arms | 0.8 | yes |
| 13 | RightHand | `right_hand` | RightLowerArm | 9 | Arms | 1.0 | yes |
| 14 | LeftUpperLeg | `left_upper_leg` | Hips | 18 | Legs | 0.3 | yes |
| 15 | LeftLowerLeg | `left_lower_leg` | LeftUpperLeg | 19 | Legs | 0.6 | yes |
| 16 | LeftFoot | `left_foot` | LeftLowerLeg | 20 | Legs | 0.8 | yes |
| 17 | LeftToes | `left_toes` | LeftFoot | 21 | Legs | 1.0 | no |
| 18 | RightUpperLeg | `right_upper_leg` | Hips | 14 | Legs | 0.3 | yes |
| 19 | RightLowerLeg | `right_lower_leg` | RightUpperLeg | 15 | Legs | 0.6 | yes |
| 20 | RightFoot | `right_foot` | RightLowerLeg | 16 | Legs | 0.8 | yes |
| 21 | RightToes | `right_toes` | RightFoot | 17 | Legs | 1.0 | no |

"Parent slot" is the anatomical parent; when it is missing on a rig the **effective parent
slot** is the nearest present ancestor (Neck missing: Head's parent is UpperChest, else Chest,
else Spine). Fingers, eyes and jaw are not animated by the pose tool (they keep their rest pose).

### 4.2 Data model (`PoseTypes.h`, written up front)

```cpp
namespace jasperengine::pose {
    enum class TransitionCurve : uint8_t { Spring, Linear, Smooth };
    struct Transition { TransitionCurve curve = Spring; float duration = 0.12f; float overshoot = 0.25f; float anticipation = 0.06f; };
    struct HumanizeSettings { bool enabled=false; bool jointLimits=true; float movingHolds=0.08f; float inbetweens=0.4f;
                              float life=0.6f; uint32_t seed=1; float headFollow=0.5f; float torsoFollow=0.4f; float springiness=0.5f; };
    struct PoseAnimationSettings { bool loop=true; Transition transition; float stepRate=0; float overlap=0.05f; float looseness=0.2f;
                                   std::array<float,4> weights{1,1,1,0}; /* BodyGroup order: spine, head, arms, legs */
                                   HumanizeSettings humanize; int frameRate=30; };
    constexpr int DOF_COUNT = 66;                       // 22 bones x 3 DOFs, index = bone*3 + k
    struct BodyPose { std::array<float, DOF_COUNT> angles{}; /* degrees, 0 = neutral */
                      Vector3 offset; /* canonical, humanScale units; y is always 0 (vertical comes from grounding) */ };
    struct PoseKey { std::string name; float hold=0.1f; bool customTransition=false; Transition transition;
                     bool grounded=true; float lift=0; /* humanScale units above the floor, used when !grounded */ BodyPose body; };
    struct PoseAnimationData { PoseAnimationSettings settings; std::vector<PoseKey> poses; std::string previewModel; };
    struct HumanPose { std::array<Quaternion, HUMAN_BONE_COUNT> rotations; /* local, relative to the actual skeleton parent bone */
                       Vector3 hipsPosition; /* hips bone local position */ };
}
```

Value ranges (clamped everywhere a value enters: UI, MCP, file load; one table, `PoseRanges.h`):

| field | range | | field | range |
|---|---|---|---|---|
| hold | 0 .. 5 s | | stepRate | 0 .. 30 (0 = off) |
| transition.duration | 0.02 .. 1.0 s | | overlap | 0 .. 0.3 s |
| transition.overshoot | 0 .. 0.9 | | looseness | 0 .. 1 |
| transition.anticipation | 0 .. 0.5 | | weights[i] | 0 .. 2 |
| lift | 0 .. 3 | | offset.x / offset.z | -5 .. 5 |
| humanize.movingHolds | 0 .. 0.5 | | humanize.inbetweens | 0 .. 1 |
| humanize.life | 0 .. 5 degrees | | humanize.seed | 0 .. 2^31-1 |
| humanize.headFollow / torsoFollow | 0 .. 1.5 | | humanize.springiness | 0 .. 1 |
| frameRate | 12 .. 120 (UI offers 24/30/60) | | angles | -180 .. 180 (wrapped) |

`PoseRanges.h` exposes `clampSettings`, `clampTransition`, `clampKey` that return the clamped
value and report each field that changed (name, requested, applied): the MCP tools echo them.

### 4.3 Canonical character frame

All joint-angle maths happens in the **canonical character frame**: `L = +X` (character's left),
`U = +Y` (up), `F = +Z` (forward), right-handed. `PoseRig` finds the rotation `C` that maps
canonical axes to model space: `up_m = +Y`; `left_m = normalize(reject(restWorldPos(LeftUpperLeg)
- restWorldPos(RightUpperLeg), up_m))`; `forward_m = cross(left_m, up_m)`;
`C = Quaternion(left_m, up_m, forward_m)` (axes as columns). A canonical world rotation `W` is
`C^-1 * W_model`; a canonical position is `C^-1 * p_model`.

### 4.4 Neutral pose

The neutral standing pose is joint angle 0. It is computed from the rest pose of each rig, so
T-pose and A-pose rigs give the same neutral:

- **Aimed bones** turn so the vector to their aim child points along the neutral direction `d`
  (table 4.5): Hips (aim: Spine), Spine (aim: first present of Chest, UpperChest, Neck, Head),
  Chest (first present of UpperChest, Neck, Head), UpperChest (Neck, Head), Neck (Head),
  Shoulders (UpperArm), UpperArms (LowerArm), LowerArms (Hand), UpperLegs (LowerLeg),
  LowerLegs (Foot).
- **Carried bones** (Head, Hands, Feet, Toes) keep their rest rotation relative to their
  effective parent slot.

Procedure, slots in table order (parents first), `R_b` = rest canonical world rotation:
1. provisional `P_b = N_p * R_p^-1 * R_b` (`P_hips = R_hips`), where intermediate non-slot bones
   between `p` and `b` keep their rest local transforms (they are part of `R_p^-1 * R_b`);
2. aimed bones: `a = R_b^-1 * (restPos(aimChild) - restPos(b))` (bone-local aim);
   `v = normalize(P_b * a)`; `N_b = fromToRotation(v, d_b) * P_b` (shortest arc);
   carried bones: `N_b = P_b`.

Neutral hips position: the rest hips position, then grounded (4.10) to the rest floor.

### 4.5 Joint angles (the rig-independent representation)

Each slot has three DOFs: two swing components and a twist about the neutral bone direction.
For slot `b` with effective parent slot `p` (character frame for hips), canonical world
rotations `W`:

`E_b = N_p * W_p^-1 * W_b * N_b^-1` (hips: `E = W_hips * N_hips^-1`).

`E_b` is a character-space rotation (as seen in the neutral pose) describing how the bone
deviates from following its parent. Swing-twist decomposition about the line of `d_b`:
`T = normalize(w, (v·d)d)` (identity when degenerate), `S = E * T^-1` with `S.w >= 0`;
swing vector `s` = axis * angle of `S` (degrees); twist angle `t` = the angle of `T` measured as
a rotation about the **positive** axis letter given in the table, wrapped to (-180, 180].

`dof1 = sign1 * dot(s, axis1)`, `dof2 = sign2 * dot(s, axis2)`, `dof3 = sign3 * t`.
Inverse: `s = (dof1/sign1)*axis1 + (dof2/sign2)*axis2`, `S = axisAngle(s)`,
`T = axisAngle(axis3, dof3/sign3)`, `E = S * T`, `W_b = W_p * N_p^-1 * E * N_b`.

Left-side and centre table (axis letters are canonical axes, `+`/`-` is the sign):

| slot | d | dof1 | dof2 | dof3 (twist) |
|---|---|---|---|---|
| hips | U | `lean` L+ (forward) | `side` F- (toward left) | `turn` U+ (turn left) |
| spine, chest, upper_chest, neck, head | U | `bend` L+ (forward) | `side` F- (toward left) | `twist` U+ (turn left) |
| left_shoulder | L | `raise` F+ (shrug) | `forward` U- | `twist` L+ |
| left_upper_arm | -U | `raise` F+ (out to the side) | `forward` L- (swing forward) | `twist` U+ (outward) |
| left_lower_arm | -U | `bend` L- (elbow flex) | `side` F+ | `twist` U+ |
| left_hand | -U | `bend` F- (toward the palm) | `side` L- (toward the thumb/forward) | `twist` U+ |
| left_upper_leg | -U | `forward` L- (hip flex) | `out` F+ (abduct) | `twist` U+ (toes out) |
| left_lower_leg | -U | `bend` L+ (knee flex) | `side` F+ | `twist` U+ |
| left_foot | F | `point` L+ (toes down) | `side` U+ (toes out) | `roll` F- (sole inward) |
| left_toes | F | `bend` L+ (curl down) | `side` U+ | `twist` F- |

Right-side slots use the same names and axis letters; `d` has its L component negated and the
sign of every DOF whose axis is U or F is negated (L-axis signs stay). With this rule a left and
a right bone with the same DOF values are exact mirror images. DOF names are
`<slot>.<dof>`, e.g. `left_upper_arm.raise`, `hips.turn`.

**Mirror** (`mirrorBodyPose`): swap the three values of each left/right pair; for centre slots
negate DOFs whose axis is U or F (`side`, `twist`, `turn`) and keep L-axis DOFs; negate
`offset.x`. Mirroring twice is the identity (exact).

**Joint limits** (`clampToLimits`, degrees, min..max; DOF order dof1/dof2/dof3; right = left):

| slot | dof1 | dof2 | dof3 |
|---|---|---|---|
| hips | -180..180 | -180..180 | -180..180 |
| spine, chest, upper_chest | -25..35 | -25..25 | -25..25 |
| neck, head | -40..45 | -35..35 | -55..55 |
| shoulder | -15..35 | -20..25 | -10..10 |
| upper_arm | -40..175 | -70..175 | -95..95 |
| lower_arm | 0..155 | -12..12 | -90..90 |
| hand | -75..80 | -25..35 | -15..15 |
| upper_leg | -35..125 | -35..70 | -45..50 |
| lower_leg | 0..155 | -8..8 | -12..12 |
| foot | -35..55 | -25..35 | -25..30 |
| toes | -60..45 | -6..6 | -6..6 |

`clampToLimits(BodyPose&) -> std::vector<int>` (indices of clamped DOFs). Limits are applied to
key poses after edits when `humanize.jointLimits` is on, and to the baked frames (section 4.8).

### 4.6 Humanoid map and auto-detect (`HumanoidMap.h/.cpp`)

```cpp
struct SkeletonBone { std::string name; int parent = -1; Vector3 position; Quaternion rotation; Vector3 scale{1,1,1}; }; // rest local TRS
struct Skeleton { std::vector<SkeletonBone> bones; };   // any order; parent = index or -1
struct HumanoidMap { std::array<std::string, HUMAN_BONE_COUNT> bones; /* "" = unmapped */ };
HumanoidMap autoDetectHumanoid(const Skeleton&, std::vector<std::string>* notes = nullptr);
bool validateHumanoid(const Skeleton&, const HumanoidMap&, std::string* error);
```

Auto-detect (deterministic, case-insensitive): normalise each name (strip a namespace/prefix up
to the last `:` or `|`; strip leading `mixamorig`, `bip01`, `bip001`, `def-`, `def_`, `j_bip_`,
`b_`, `bone_`; split camelCase and on `_ . - space`). Side tokens: `left`/`l` and `right`/`r` as a
whole token (`upper_arm.L`, `UpperArm_L`, `L_UpperArm`, `LeftArm`, `clavicle_l`). Body-part
synonyms (first match wins, in this priority):

- hips: `hips`, `pelvis`, `hip` (centre, no side); spine chain: every centre bone named
  `spine*`, `chest`, `upperchest`, `torso` between hips and neck in hierarchy order; the first
  is Spine, then Chest, then UpperChest; when there are more than three, Spine = first,
  Chest = middle, UpperChest = last before the neck. neck: `neck*` (the first); head: `head`.
- shoulder: `shoulder`, `clavicle`, `collar`; upper arm: `upperarm`, `upper arm`, `arm` (when
  not `forearm`/`lowerarm`), `uparm`; lower arm: `forearm`, `lowerarm`, `lower arm`, `elbow`;
  hand: `hand`, `wrist`.
- upper leg: `upleg`, `upperleg`, `upper leg`, `thigh`; lower leg: `leg` (not up/upper), `lowerleg`,
  `calf`, `shin`, `knee`; foot: `foot`, `ankle`; toes: `toe*`, `ball`.
- Twist/roll helper bones (`twist`, `roll`, `helper`, `ik`, `pole`, `target`, `end`, `nub`) are
  never mapped.

Then a structural pass fixes gaps: Hips = the lowest common ancestor of both upper legs if
unmapped; a required slot still empty -> `validateHumanoid` fails with the list of missing slots.
Notes report every slot mapped by the structural pass and every ambiguity resolved.
Test cases (section 6) cover Mixamo, Blender/Rigify-like, Unreal mannequin, VRM and
Unity-style names.

### 4.7 Rig (`PoseRig.h/.cpp`) and maths helpers (`PoseMath.h`)

`PoseMath.h`: inline radian helpers independent of `Angle::defaultToRad`: `axisAngleRad`,
`fromToRotation`, `angleBetween`, `slerpUnclamped(a, b, t)` (t may be < 0 or > 1),
`powRotation(q, t)` (shortest-path power), `swingTwist`, `rotateTowards(from, to, maxRad)`,
`smoothstep`, `smootherstep`, `positiveFmod`.

```cpp
class JASPERENGINE_API PoseRig {
public:
    bool build(const Skeleton&, const HumanoidMap&, std::string* error);
    bool has(HumanBone) const;  int boneIndex(HumanBone) const;  HumanBone effectiveParent(HumanBone) const;
    const Skeleton& skeleton() const;  const HumanoidMap& humanoid() const;
    Quaternion characterRotation() const;                   // C (4.3)
    float humanScale() const;                               // neutral hips height above the floor / 0.95
    float floorHeight() const;                              // model-space height of the rest pose's lowest point
    const HumanPose& restPose() const;  const HumanPose& neutralPose() const;
    // FK: local slot rotations/hips position -> model-space world rotation/position of every skeleton bone
    void computeWorld(const HumanPose&, std::vector<Quaternion>& worldRot, std::vector<Vector3>& worldPos) const;
    void computeWorldMatrices(const HumanPose&, std::vector<Matrix4>& world) const;
    // joint angles (4.5) and realisation
    BodyPose toBodyPose(const HumanPose&) const;            // offset from hips horizontal displacement vs neutral
    HumanPose fromBodyPose(const BodyPose&, const PoseSkin* skin, bool grounded, float lift) const; // grounded via 4.10
    void setSkin(std::shared_ptr<PoseSkin>);  const PoseSkin* skin() const;   // optional exact lowest point
};
```

`humanScale` is the neutral hips height above the floor divided by 0.95, so offsets and lifts
are roughly metres for a human-sized character. `fromBodyPose(toBodyPose(p))` reproduces `p`'s
rotations within 1e-4 rad and its hips horizontal position within 1e-4 * humanScale; vertical
is re-derived by grounding.

### 4.8 Tween, feel and sampling (`PoseTween.h/.cpp`, `PoseSampler.h/.cpp`)

**Ease** `float ease(float x, const Transition&)`, `x = (t - start) / duration`:
- `x <= 0` -> 0. Linear: `min(x, 1)`. Smooth: smootherstep(0, 1, x).
- Spring: `a = anticipation`, `p = a > 0 ? 0.3 : 0`. Wind-up: `x < p` -> `-a * (1 - cos(pi*x/p)) / 2`.
  Spring part with `u = (x - p) / (1 - p)`: `Mp = clamp(overshoot, 0.001, 0.9)`,
  `k = -ln(Mp) / pi`, `w = pi - atan(1/k)`,
  `y(u) = 1 - exp(-k*w*u) * (cos(w*u) + k*sin(w*u))`; result `-a + (1 + a) * y(u)`.
  Properties (tested): minimum `-a`; value exactly 1 at `x = 1`; peak `1 + Mp*(1+a)`
  (overshoot 0 -> Mp 0.001, effectively none); tends to 1; continuous with zero slope at `x = p`.

**Feel presets** (`FeelPreset` table, name -> transition + stepRate + overlap + looseness):

| preset | curve | duration | overshoot | anticipation | stepRate | overlap | looseness |
|---|---|---|---|---|---|---|---|
| Snappy | spring | 0.12 | 0.25 | 0.06 | 0 | 0.05 | 0.2 |
| Cartoon | spring | 0.10 | 0.35 | 0.12 | 0 | 0.10 | 0.65 |
| Rubbery | spring | 0.20 | 0.50 | 0.10 | 0 | 0.10 | 0.5 |
| Stop-motion | spring | 0.10 | 0.20 | 0.0 | 12 | 0.04 | 0 |
| Heavy | spring | 0.35 | 0.08 | 0.15 | 0 | 0.08 | 0.1 |
| Linear | linear | 0.25 | 0 | 0 | 0 | 0 | 0 |
| Soft | smooth | 0.30 | 0 | 0 | 0 | 0.06 | 0.15 |

A preset "matches" when every value is equal within 1e-4 (the UI lights its chip).

**Timeline**: `n` poses; segment `s` goes from pose `s` to pose `s+1` (the last segment, from
pose `n-1` back to pose 0, exists only when looping). `start[s] = key + hold_s`,
`key = start[s] + dur_s`, with `dur_s = TransitionOut(s).duration` (the pose's own transition
when `customTransition`, else the animation's). Length = sum of holds + sum of segment
durations. One pose: length = its hold (minimum 1 frame), the pose is static.

**Sample** `void sample(const SamplerInput&, float t, HumanPose& out)`; `SamplerInput` holds the
realised `HumanPose` per key, the settings and the timeline. Per bone `b` (group `g`, depth `D`):
1. Choppiness: `stepRate > 0` -> `t = floor(t*stepRate + 1e-4) / stepRate`.
2. Overlap delay `tau = t - overlap * weight[g] * D`; looseness `l = clamp01(looseness * weight[g] * D)`.
3. `tau = loop ? positiveFmod(tau, len) : max(tau, 0)`.
4. `r = key[0].rotations[b]`; for cycle `c` in `loop ? {-1, 0} : {0}` and each segment `s`:
   `local = tau - start[s] - c*len`; `tr` = the segment transition, loosened if `l > 0`
   (`duration *= 1 + 1.5*l`, `overshoot = lerp(overshoot, 0.7, l)`);
   `e = ease(local / max(tr.duration, 1e-4), tr)`; with humanize on and `movingHolds > 0`:
   `hp = hold_s > 1e-4 ? smoothstep(0, 1, (local + hold_s) / hold_s) : (local >= 0 ? 1 : 0)`,
   `e = movingHolds * hp + (1 - movingHolds) * e`; skip when `e == 0`;
   `r = r * powRotation(key[s].rot[b]^-1 * key[s+1].rot[b], e)`;
   hips (`b == 0`) also `pos += (key[s+1].hipsPosition - key[s].hipsPosition) * e`.
5. Life (humanize, `life > 0`, `b != hips`, `g != Legs`): `r = r * lifeRotation(b, t, len, life, seed)`:
   `cycles = max(1, round(len * 0.7))`; per axis `i` (x, y, z):
   `sum_{k=1..3} sin(2*pi*cycles*k*t/len + phase(b,i,k)) / k`, `phase = 2*pi*hash01(seed, b, i, k)`
   with `hash01` a documented integer hash (splitmix64 of the packed inputs, top 24 bits / 2^24);
   angle = sum * life / 1.83 degrees about the bone-local axis `i`; applied x then y then z.
6. `out.rotations[b] = normalize(r)`.
When every `e = 1` the product telescopes to the last pose exactly; a full loop multiplies to
identity, so loops are seamless.

**Frames** `std::vector<HumanPose> frames(const SamplerInput&, int fps)`:
`count = max(1, round(len * fps))`, frame `f` at `t = f * len / count` for `f = 0..count`.
Then, with humanize: in-betweens (`inbetweens > 0`): `maxDeg = lerp(180, 12, inbetweens) * 30 / fps`;
a running state per bone follows the frames with `rotateTowards(state, frame, maxDeg)` (hips
position not limited); loops run one warm-up pass first and force the last frame equal to the
first. Then follow-through (4.9), then joint limits (via `toBodyPose` / `clampToLimits` /
`fromBodyPose` keeping the frame's hips position) when `jointLimits`. Frames are the single
source for preview, bake and runtime playback.

**Auto timing** `void autoTiming(PoseAnimationData&, const std::vector<HumanPose>& realised, float humanScale)`:
motion of segment `i` = mean angle between the poses' bone rotations (degrees) + 100 * hips travel
/ humanScale; `mean` = mean motion; `holdBase = max(mean hold, 0.06)`;
`hold_i = holdBase * clamp((motion_incoming_i / mean)^2, 0.1, 3)`;
`dur_i = settings.transition.duration * lerp(0.6, 1.6, clamp01(motion_i / mean / 2))`, every pose
gets a custom transition with that duration; then everything is rescaled so the total length is
unchanged; with `stepRate > 0` holds and durations snap to the `1/stepRate` grid (minimum one
step); durations clamp to 0.02..1.0. All-zero motion leaves the timing unchanged.

### 4.9 Follow-through (`PoseSecondary.h/.cpp`)

Input: the frames, the rig, fps, loop, humanize. Hips canonical positions per frame divided by
humanScale; acceleration by central differences `(p[f+1] - 2p[f] + p[f-1]) * fps^2` (wrapping
when looping, one-sided zero at the ends otherwise). Two springs (torso 3.5 Hz, head 2.5 Hz),
`zeta = lerp(0.9, 0.2, springiness)`, `x'' = -omega^2 x - 2 zeta omega x' - acc`, semi-implicit
Euler, 4 substeps per frame; loops run two passes and copy frame 0 into the last frame.
Tilt per spring with `K = 300 * amount` (`amount` = torsoFollow / headFollow):
`lean = clamp(K * (x.z - x.y), -35, 35)` degrees (about canonical L, + forward),
`side = clamp(K * x.x, -35, 35)` degrees (about canonical F with sign -, + toward the left).
Apply as world-space rotations, parent before child, to the chain Spine 0.3, Chest 0.35,
UpperChest 0.35 (torso tilt; a missing bone's share goes to the next present one) and Neck 0.4,
Head 0.6 (head tilt), converting back to local rotations. Effects (tested): accelerating forward
leans the torso back; a drop (downward acceleration) tilts the chin up; landing leans forward.

### 4.10 IK and grounding (`PoseIK.h/.cpp`, `PoseSkin.h/.cpp`)

```cpp
enum class Limb : uint8_t { LeftArm, RightArm, LeftLeg, RightLeg };
bool solveLimbIK(const PoseRig&, HumanPose&, Limb, const Vector3& targetModel, bool keepEndWorldRotation);
void moveHipsKeepFeet(const PoseRig&, HumanPose&, const Vector3& newHipsWorldModel);
float lowestPoint(const PoseRig&, const HumanPose&);   // model-space height along up: skin if set, else sole estimate
void ground(const PoseRig&, HumanPose&, float lift /* humanScale units */);   // moves hips along up so lowest = floor + lift*humanScale
```

Two-bone IK (A = upper, B = lower, C = end): clamp the reach to
`[|lab - lbc| + 1e-4, lab + lbc - 1e-4]`; bend axis = `cross(C - A, B - A)`, or when the limb is
straight `cross(C - A, pole)` with pole = character forward for legs, -forward for arms; rotate B
by the change of the interior angle about the bend axis, then A by `fromTo(C - A, T - A)`;
`keepEndWorldRotation` restores C's world rotation (feet), otherwise C keeps its local rotation
(hands). Reach beyond the limb clamps (the end stops at full extension along the target line).

Sole estimate: at build, for Feet and Toes `sole_k = restHeight(bone_k) - restLowest` where
`restLowest` = the skin's lowest rest vertex when a skin is set, else the minimum of those bone
heights; `estimate = min_k(height_k - sole_k)`.

`PoseSkin`: a CPU skinning sample of the character mesh: up to 6000 vertices (deterministic
stride over all skinned vertices of all skinned meshes), each with up to 4 (skeleton bone index,
weight) pairs, and the inverse bind matrix per skeleton bone; `lowest(worldMatrices)` = min over
samples of the up component of `sum w_j * (World_j * InvBind_j) * v`. Phase 2 builds it from the
engine's mesh buffers.

## 5. Build and tests

- Sources are added to `engine/CMakeLists.txt` (explicit list, next to `core/action/*`).
- New option `JASPERENGINE_POSE_TESTS` (OFF by default) builds `jasperengine-pose-tests`, a
  googletest executable that compiles only `engine/core/pose/*.cpp` and the math sources it
  needs (no GPU, no window), from `engine/tests/pose/`. It must build both from the editor
  top-level build (`cmake -S . -B build -DJASPERENGINE_POSE_TESTS=ON`) and the standalone engine
  build, and run in under 10 s.
- Fixture rigs for tests are built in C++ test helpers (`engine/tests/pose/TestRigs.h`): a
  T-pose rig with Mixamo names (glTF axes), an A-pose rig with Blender names and different
  proportions and extra non-slot bones between slots (twist bones, a 4-bone spine), a rig with
  optional slots missing (no chest, upper chest, neck, shoulders, toes), and a rig whose model
  space faces -Z.

## 6. Acceptance tests (all in `jasperengine-pose-tests`)

1. Bone table: names unique, parents precede children, mirror is an involution, groups/depths
   as in 4.1; `humanBoneFromName` round trip.
2. Auto-detect maps all four fixture rigs fully, plus name lists for Mixamo, Rigify (`DEF-`),
   Unreal (`pelvis`, `spine_01..05`, `clavicle_l`, `upperarm_l`, `lowerarm_l`, `thigh_l`,
   `calf_l`, `ball_l`), VRM (`J_Bip_C_Hips`, `J_Bip_L_UpperArm`...) and Unity names; twist bones
   are never mapped; a rig missing a required slot fails validation naming the slot.
3. Character frame: forward is +Z for glTF rigs and -Z for the flipped rig.
4. Neutral: T-pose and A-pose rigs give neutral poses whose bone directions match `d` within
   0.5 degrees; `toBodyPose(neutralPose)` is all zeros (< 1e-3 degrees).
5. Round trips: random body poses (angles within limits) -> human pose -> body pose within
   1e-3 degrees on all four rigs; human -> body -> human within 1e-4 rad.
6. Semantics on the T-pose rig: `left_upper_arm.raise = 90` puts the left hand at shoulder
   height to the left; `left_upper_leg.forward = 90` puts the left foot forward at hip height;
   `left_lower_leg.bend = 90` moves the foot back; `hips.turn = 90` makes forward point to +X;
   `head.bend = 30` moves the head forward.
7. Cross-rig: the same body pose realised on the T-pose and the A-pose rig gives the same
   normalised bone directions (within 1 degree).
8. Mirror: twice = identity (exact); `mirror(pose)` realised on the T-pose rig equals the
   reflection of `pose` realised (positions mirrored in x within 1e-4 * humanScale).
9. Limits: out-of-range DOFs clamp to the table and are reported; in-range untouched.
10. Ease: the spring properties of 4.8; Linear/Smooth ignore overshoot/anticipation.
11. Sampler: telescoping (all e = 1 gives the last key exactly); loop seam (sample(0) ==
    sample(len)); overlap delays deep bones; stepRate quantises; moving holds drift during a
    hold; life loops seamlessly; frames count and last == first when looping; in-betweens cap
    the per-frame rotation; one-pose animations are static.
12. Auto timing: preserves total length, larger motion gets a longer snap, snaps to the step
    grid, zero motion is a no-op.
13. Follow-through: the three effects of 4.9 on synthetic hips motion; loop steady state.
14. IK: reachable targets reached within 1e-4 * humanScale on all limbs; unreachable targets clamp
    on the target line; feet keep world rotation; the pole decides the bend side; moving the
    hips keeps both feet within 1e-4 * humanScale.
15. Grounding: grounded poses touch the floor (lowest = floor within 1e-5); lift raises by
    `lift * humanScale`; skin lowest point beats the sole estimate on a tilted foot (synthetic
    skin).

## 7. Out of scope for phase 1

Files, ECS components, editor, MCP, rendering. Phase 2 builds `Skeleton` from a
`ModelComponent` and `PoseSkin` from mesh buffers.
