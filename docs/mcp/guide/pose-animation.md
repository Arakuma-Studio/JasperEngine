# Pose animation (`.poseanim`)

Pose-to-pose character animation for humanoid models: a few **key poses** written as joint
angles, snapped between with springy transitions ("feel"), optionally humanised (moving holds,
in-betweens, sway, follow-through, joint limits). The same asset plays live on a model (Pose
Animator component, Lua `PoseAnimator`) or bakes to a normal Animation.

Tools: `get_pose_animation` (read) and `edit_pose_animation` (every change, one project undo step
per call: `undo {scope: "project"}`). The `.poseanim` file is JSON and safe to read, but edit it with
the tool: it validates, clamps, echoes what it clamped and keeps the undo history.

## The loop

1. `get_pose_animation {path}`: settings, timing (`start_time` of every pose, `length`) and the poses.
   With a rig (the file's `preview_model`, or `model`: a model GameObject of the scene) every pose has
   `metrics`: foot heights and contacts, `balanced` / `balance_margin`, `limit_hits`; `rig` says which
   model it is, how many slots are `mapped` (`bones`: slot -> bone), the `missing` required slots and
   `human_scale` (or the `error` that kept the model from being a usable humanoid).
2. `edit_pose_animation {path, ops: [...]}`: change several poses and settings in one call.
3. Check the metrics of the touched poses the result returns (and the motion in play mode or after
   baking). Fix, repeat.

Start a new file with `create`: `{path: "assets/anim/walk.poseanim", create: {preview_model:
"assets/models/hero.gltf"}, ops: [{op: "add_pose", name: "Contact R", angles: {...}}, ...]}`.
Keep `.poseanim` files inside the assets folder: games load them from there.

## Angles: degrees from a neutral standing pose

Every pose is a set of **joint angles in degrees**; 0 everywhere is the **neutral standing pose**:
standing straight, arms hanging down at the sides (palms toward the body), legs straight, feet
forward. It is NOT a T-pose: a T-pose is `left_upper_arm.raise: 90, right_upper_arm.raise: 90`.
Angles are rig-independent: the same pose works on any humanoid (T-pose or A-pose models, any bone
names).

A DOF name is `<slot>.<dof>`. Right-side slots have the same DOF names, limits and meanings as the
left ones: equal values on both sides are exact mirror images (`right_upper_arm.raise: 90` lifts the
right arm out to the right). Centre slots: positive `side` tilts toward the character's left,
positive `twist`/`turn` turns to the character's left. Values outside a limit are kept (and reported
in `limit_hits`) unless `humanize.joint_limits` is on (the default, independent of
`humanize.enabled`), which clamps added and updated poses (echoed in `clamped`) and the frames.

| slot | dof1 | dof2 | dof3 (twist) |
|---|---|---|---|
| `hips` | `lean` + forward (-180..180) | `side` + toward the left (-180..180) | `turn` + turn left (-180..180) |
| `spine`, `chest`, `upper_chest` | `bend` + forward (-25..35) | `side` + toward the left (-25..25) | `twist` + turn left (-25..25) |
| `neck`, `head` | `bend` + forward / chin down (-40..45) | `side` + toward the left (-35..35) | `twist` + turn left (-55..55) |
| `left_shoulder` | `raise` + shrug up (-15..35) | `forward` + forward (-20..25) | `twist` (-10..10) |
| `left_upper_arm` | `raise` + out to the side, 90 = horizontal (-40..175) | `forward` + swing forward (-70..175) | `twist` + outward (-95..95) |
| `left_lower_arm` | `bend` + elbow flex (0..155) | `side` (-12..12) | `twist` (-90..90) |
| `left_hand` | `bend` + toward the palm (-75..80) | `side` + toward the thumb (-25..35) | `twist` (-15..15) |
| `left_upper_leg` | `forward` + hip flex, leg forward (-35..125) | `out` + out to the side (-35..70) | `twist` + toes out (-45..50) |
| `left_lower_leg` | `bend` + knee flex, foot back (0..155) | `side` (-8..8) | `twist` (-12..12) |
| `left_foot` | `point` + toes down (-35..55) | `side` + toes out (-25..35) | `roll` + sole inward (-25..30) |
| `left_toes` | `bend` + curl down (-60..45) | `side` (-6..6) | `twist` (-6..6) |

The 22 slots: `hips`, `spine`, `chest`, `upper_chest`, `neck`, `head`, `left_/right_` +
`shoulder`, `upper_arm`, `lower_arm`, `hand`, `upper_leg`, `lower_leg`, `foot`, `toes`. A rig may
lack the optional ones (`chest`, `upper_chest`, `neck`, shoulders, toes): their angles are ignored.
Fingers, eyes and jaw keep their rest pose.

Useful numbers: a relaxed arm is `upper_arm.raise` 8..15 and `lower_arm.bend` 10..20; a walk
contact has the front `upper_leg.forward` about 25 and the back one about -15; a crouch is
`upper_leg.forward` 90..110 with `lower_leg.bend` 100..130 and `foot.point` -20..-35 (the feet stay flat).

## Poses

| field | meaning |
|---|---|
| `name` | label (not unique) |
| `hold` | seconds the pose is held before its transition out (0..5) |
| `transition` | `null` = the animation's transition; an object = this pose's own transition out (`{curve, duration, overshoot, anticipation}`; a partial object starts from the transition the pose uses now) |
| `grounded` | true: the lowest point of the body touches the floor (automatic, never set a height by angles) |
| `lift` | when `grounded` is false: height above the floor in body units (about metres for a human; 0..3) |
| `offset` | `{left, forward}`: hips horizontal offset in body units (-5..5), the character's own axes |
| `angles` | `{dof: degrees}`; only DOFs at least 0.05 degrees away from 0 are listed |

`start_time` (read only) is when the pose is reached. Timing: pose 0 at 0; each pose holds `hold`,
then moves to the next one in its transition's `duration`; a looping animation also moves from the
last pose back to the first. `length` = all holds + all transitions; a one-pose animation is static.

## Ops (`edit_pose_animation`)

`ops` run in order on a copy; if one fails nothing is written and the error names it
(`ops[2].angles: unknown DOF 'left_elbow.bend' (did you mean 'left_lower_arm.bend'?)`). Indices are
0-based and refer to the poses as the earlier ops of the same call left them.

| op | arguments |
|---|---|
| `add_pose` | `after` (-1 = first; default last), `name`, `from` (pose to copy; default `after`, neutral when empty), `blend_to` + `blend` (0..1: angles, offset and lift lerped toward that pose: in-betweens), `hold`, `grounded`, `lift`, `offset`, `angles`, `add_angles`, `transition` |
| `update_pose` | `index` + any of `name`, `hold`, `grounded`, `lift`, `offset`, `angles`, `add_angles`, `transition` (`null` clears the custom one). Metadata never changes angles or offset |
| `delete_pose` | `index` (the last pose cannot be deleted) |
| `move_pose` | `index`, `to` |
| `mirror_pose` | `index`, `as_new` (true: a mirrored copy inserted after it), `name`. Exact: mirroring twice gives the pose back |
| `set_settings` | any of `loop`, `frame_rate` (12..120), `transition`, `step_rate`, `overlap`, `looseness`, `weights {spine, head, arms, legs}`, `humanize {...}` (partial objects) |
| `apply_preset` | `name`: one of the feel presets below |
| `auto_timing` | holds and transition durations from how far each pose moves (bigger moves get longer snaps); the total length stays |
| `set_preview_model` | `model`: the model file previews and metrics use (`""` clears) |
| `set_humanoid` | `model`, `bones {slot: bone name}`: fixes the auto-detected bone mapping of a model, written to its `.humanoid` sidecar (`""` = slot unmapped, `null` = back to auto-detect) |

`angles` sets absolute degrees for the DOFs it names, `add_angles` adds to them; other DOFs keep
their values. Out-of-range values are clamped and listed in `clamped: [{field, requested, applied}]`.
`bake {model, name}` (after the ops; `name` defaults to the file name) writes the frames to an
Animation of a model GameObject (one RotateTracks per mapped bone and a TranslateTracks for the
hips, one key per frame at the frame times, step keys when `step_rate` > 0; its own scene undo step,
`undo {scope: "scene"}`; baking again rewrites the keys of the same tracks). Play it with
`Model:playAnimation(name)`. `bake` alone (no ops) re-bakes after edits.

## Feel (`feel`) and humanize (`humanize`)

- `transition.curve`: `spring` (overshoot and wind-up), `linear`, `smooth` (no overshoot).
  `duration` (0.02..1 s): the snap time, the target is first reached then; `overshoot` (0..0.9): how
  far past the target the spring swings; `anticipation` (0..0.5): wind-up before the move.
- `step_rate` (0..30): choppiness, poses per second like stop motion (0 = off). `overlap` (0..0.3 s):
  how much later the deepest bones (hands, feet, head) arrive. `looseness` (0..1): deep bones also
  swing further. `weights` (0..2 per body group: spine, head, arms, legs) scale overlap and looseness.
- `humanize`: `enabled`, `joint_limits` (clamp poses and frames to the limits above),
  `moving_holds` (0..0.5: a held pose keeps drifting toward the next), `inbetweens` (0..1: caps the
  rotation per frame), `life` (0..5 degrees of idle sway), `seed`, `head_follow` and `torso_follow`
  (0..1.5: the head and torso lag behind hips accelerations), `springiness` (0..1).

Presets (`apply_preset`, `create.preset`; `preset` in the result names the one matching):

| preset | curve | duration | overshoot | anticipation | step_rate | overlap | looseness |
|---|---|---|---|---|---|---|---|
| Snappy | spring | 0.12 | 0.25 | 0.06 | 0 | 0.05 | 0.2 |
| Cartoon | spring | 0.10 | 0.35 | 0.12 | 0 | 0.10 | 0.65 |
| Rubbery | spring | 0.20 | 0.50 | 0.10 | 0 | 0.10 | 0.5 |
| Stop-motion | spring | 0.10 | 0.20 | 0.0 | 12 | 0.04 | 0 |
| Heavy | spring | 0.35 | 0.08 | 0.15 | 0 | 0.08 | 0.1 |
| Linear | linear | 0.25 | 0 | 0 | 0 | 0 | 0 |
| Soft | smooth | 0.30 | 0 | 0 | 0 | 0.06 | 0.15 |

## Metrics

Per pose, on the rig: `feet.left` / `feet.right` `{height, contact}` (the sole's height above the
floor in body units; contact at 0.02 or less), `balanced` (the centre of mass is above the support
area of the feet in contact), `balance_margin` (body units, negative = outside, falling), and
`limit_hits` (DOFs outside the joint limits). A grounded pose always has its lowest point on the
floor: an unbalanced standing pose needs the hips (`offset`) over the supporting foot, not a lift.

## In the editor

People pose in the Scene view while the Pose Animation window poses a character: hand and foot
spheres drag the limb (IK, feet snap to the floor), the hips cube moves the body with the feet
planted, joint dots show rotation rings that stop at the joint limits (Shift: horizontal drag,
Ctrl/Cmd: 5 degree ring steps, E: Global Edit). The floor disc shows contact rings and the centre
of mass; ghosts show the previous/next pose. Each drag is one project undo step, like your edits.

## Humanoid mapping

Bones are mapped to the slots automatically from their names (Mixamo, Blender/Rigify, Unreal, VRM,
Unity names; twist, roll and end bones are never mapped). `rig` in the result lists the `missing`
slots. When a mapping is wrong, `set_humanoid` stores the fix next to the model
(`models/hero.gltf.humanoid`); it ships with the game like any asset.

## Common mistakes

- Treating 0 as a T-pose: 0 is standing with the arms down.
- Mirroring by hand: use `mirror_pose` (exact, also negates the centre `side`/`twist` and the
  offset `left`).
- Setting heights through angles or `lift` on standing poses: grounding is automatic; `lift` only
  for airborne poses (`grounded: false`).
- Re-sending every angle to change one: `update_pose` with only the changed DOFs (or `add_angles`).
- Swings over 180 degrees: the two swing DOFs of a slot form one swing whose length is
  sqrt(dof1^2 + dof2^2); past 180 it is the same rotation as a shorter swing the other way, so it
  does not come back as written (`upper_arm.raise: 150, forward: 150` reads back differently). Keep
  each slot's combined swing under 180, or twist instead.
- Even timing everywhere: vary holds, or run `auto_timing`, then adjust.
- Editing the `.poseanim` file by hand: no clamping, no echo, no undo step.
