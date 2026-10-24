# Pose animation (`.poseanim`)

Pose-to-pose character animation for humanoid models: a few **key poses** written as joint
angles, snapped between with springy transitions ("feel"), optionally humanised (moving holds,
in-betweens, sway, follow-through, joint limits). The same asset plays live on a model (Pose
Animator component, Lua `PoseAnimator`) or bakes to a normal Animation.

Tools: `get_pose_animation` (read), `edit_pose_animation` (every change, one project undo step
per call: `undo {scope: "project"}`) and `render_pose_animation` (pictures of the poses or the
motion). The `.poseanim` file is JSON and safe to read, but edit it with the tool: it validates,
clamps, echoes what it clamped and keeps the undo history.

## The loop

1. `get_pose_animation {path}`: settings, timing (`start_time` of every pose, `length`) and the poses.
   With a rig (the file's `preview_model`, or `model`: a model GameObject of the scene) every pose has
   `metrics`: foot heights and contacts, `balanced` / `balance_margin`, `limit_hits`; `rig` says which
   model it is, how many slots are `mapped` (`bones`: slot -> bone), the `missing` required slots and
   `human_scale` (or the `error` that kept the model from being a usable humanoid).
2. `edit_pose_animation {path, ops: [...]}`: change several poses and settings in one call (not one
   call per pose).
3. Check the metrics of the touched poses the result returns: standing poses balanced with a foot in
   contact, airborne poses lifted, no `limit_hits`. Fix, repeat.
4. Look: `render_pose_animation {path}` draws the key poses (front and side rows: render them after
   every batch of pose changes), then `mode: "motion"` (evenly spaced `frames`, side row) for the timing
   and feel, or `mode: "times"` (`times` in seconds): the frames the bake and the runtime play, with feet
   in contact marked `[L]`, `[R]`, `[LR]` in the labels. The skeleton overlay is coral for the character's left limbs and cyan for the
   right (the front view faces you: its left is on image right); the orange line is the floor; every
   view row has one scale, so a jump shows its height. The result also has per-column `metrics`. Each
   image costs tokens: render the poses you changed (`poses`), few `frames`, one or two `views`.
   For detail keep it to 8 columns or fewer per row (clients downscale images wider than about 1568 px).

Start a new file with `create`: `{path: "assets/anim/walk.poseanim", create: {preview_model:
"assets/models/hero.gltf"}, ops: [{op: "add_pose", name: "Contact R", angles: {...}}, ...]}`, or
from a template: `create: {template: "Walk", preview_model: ...}` (see Templates).
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
Child slots follow their parent: `hips.lean` tilts the legs too, so a bow or a crouch leaning
forward by L adds L to both `upper_leg.forward`. A foot is flat when `foot.point` is about
(thigh angle from vertical) - `lower_leg.bend`. A forearm points up from an arm out to the side
(waving) with `upper_arm.twist` about 60..90: the elbow bends in the arm's twisted plane. Keep
standing knees at 2 or more (soft knees) so the frames stay off the 0 limit.

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
| `apply_template` | `name`, `mode`: `replace` (default: the template's settings and poses; the preview model stays) or `append` (its poses after the existing ones) |
| `save_template` | `name`, `description`, `category` (default "User"): the animation as it is at this op becomes a project template, written in the call's undo step |

`angles` sets absolute degrees for the DOFs it names, `add_angles` adds to them; other DOFs keep
their values. Out-of-range values are clamped and listed in `clamped: [{field, requested, applied}]`.
`bake {model, name}` (after the ops; `name` defaults to the file name) writes the frames to an
Animation of a model GameObject (one RotateTracks per mapped bone and a TranslateTracks for the
hips, one key per frame at the frame times, step keys when `step_rate` > 0; its own scene undo step,
`undo {scope: "scene"}`; baking again rewrites the keys of the same tracks). Play it with
`Model:playAnimation(name)`; names are per model (the Hero and the Robot can both have "walk"). `bake`
alone (no ops) re-bakes after edits; `bake` without `path` bakes what the model's Pose Animator plays
(the Inspector's Bake button). To play it live instead, attach a Pose Animator:
`add_component {entity: "Hero", component: "poseanimator", values: {poseanimator: {animation:
"assets/anim/walk.poseanim", autoplay: true}}}` (the same project-relative path the pose tools take).
`model` is a model file (project-relative path) inside ops (`set_preview_model`, `set_humanoid`) and
a model GameObject of the scene in `get` / `render` / `bake`; `rig.model` is that file or `{id, name, path}`.

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

## Templates

Starter animations for any humanoid. `get_pose_animation {templates: true}` (no `path`) lists them:
`[{name, category, description, poses, loop, user}]`. Built-in: Idle (Basics); Walk, Run, Sneak,
Cartoon Walk (Locomotion, in place); Jump, Sit Down, Punch (Actions, once); Wave, Bow (Gestures).
Project templates (`user: true`) are files in `<project>/pose-templates/` written by
`save_template`; one named like a built-in replaces it. Names are case-insensitive. A template's own
pose transitions (Jump's moves into floor contacts) are part of its feel and are not warned about. Start from the
closest template and adapt it rather than posing from zero.

## Metrics

Per pose, on the rig: `feet.left` / `feet.right` `{height, contact}` (the sole's height above the
floor in body units; contact at 0.02 or less), `balanced` (the centre of mass is above the support
area of the feet in contact), `balance_margin` (body units, negative = outside, falling), and
`limit_hits` (DOFs outside the joint limits). A grounded pose always has its lowest point on the
floor: an unbalanced standing pose needs its centre of mass over the supporting foot (bring that
foot under the body or lean the torso over it; `offset` moves the feet too), not a lift.

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
- Forgetting that `add_pose` copies the pose before it (angles, offset, `grounded`, `lift`, the
  custom transition): set `grounded: true` after an airborne pose, `transition: null` after a pose
  with its own, and name every DOF the new pose changes back.
- Mirroring by hand: use `mirror_pose` (exact, also negates the centre `side`/`twist` and the
  offset `left`).
- Setting heights through angles or `lift` on standing poses: grounding is automatic; `lift` only
  for airborne poses (`grounded: false`).
- Fixing balance with `offset`: it moves the feet with the hips. Bring the supporting foot under the
  body instead (`upper_leg.out` negative on the standing leg), or lean the torso over it.
- Forgetting the arms weight: `weights.arms` 0 turns off the arms' overlap and looseness (stiff
  arms); `weights.legs` is 0 by default so planted feet do not slide.
- Re-sending every angle to change one: `update_pose` with only the changed DOFs (or `add_angles`).
- Swings over 180 degrees: the two swing DOFs of a slot form one swing whose length is
  sqrt(dof1^2 + dof2^2); past 180 it is the same rotation as a shorter swing the other way, so it
  does not come back as written (`upper_arm.raise: 150, forward: 150` reads back differently). With
  `joint_limits` on, the combined swing is capped at 179.5 (both DOFs scaled, listed in `clamped`).
  Keep each slot's combined swing under 180, or twist instead.
- Even timing everywhere: vary holds, or run `auto_timing`, then adjust.
- Changing the feel after `auto_timing`: it gives every pose its own transition, which then ignores
  `apply_preset` / `set_settings` `transition` (the result warns); add `clear_custom: true` to clear them.
- Editing the `.poseanim` file by hand: no clamping, no echo, no undo step.
