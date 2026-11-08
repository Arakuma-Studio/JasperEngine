# Animator (`.controller`, `.mask`, `.montage`)

Unity's Mecanim on the engine's clips: an **Animator** component plays an **Animator Controller**
(`.controller`): float / int / bool / trigger parameters, layers (override or additive, an Avatar
Mask, a weight), a state machine per layer (states, sub-state machines, Entry / Any State / Exit,
transitions with conditions, exit times and interruption), blend trees (1D, 2D simple
directional, 2D freeform directional and cartesian, direct), montages on slots, an IK stack and
root motion. Its clips are the Animation Player's: `.anim` files and model clips
`assets/models/hero.gltf#Walk` (`jasper://docs/animation`). A clip made on another
humanoid model plays retargeted, and humanoid clips (`.anim` version 2) play on any humanoid: the
one retarget path the Animation Player's Retarget entries use too (the models' `.humanoid`
Avatars, `set_humanoid`; humanoid clips: `jasper://docs/animation`).

The Animation Player stays the simple path (a list of clips played by name, on layers with masks:
`jasper://docs/animation`); the Animator is the state machine. Both can sit on one GameObject: the
Animator owns the bones its clips animate and runs after the player, so it wins where both write.
A **disabled** Animation Player beside an Animator hands over to it: its `play`, `crossFade`,
`stop`, parameter and state calls go to the Animator (what upstream's player-with-a-controller
scripts call; scenes from before the merge get that Animator on load, `get_animation {entity}`
says `animator.hands_over`).

Tools (group `animation`): `get_animator` (read), `edit_animator_controller`, `edit_avatar_mask`,
`edit_montage` (each call one project undo step: `undo {scope: "project"}`), `control_animator`
(drive it: play mode or the edit preview) and `sample_animation_pose` (bone world transforms). The
files are canonical JSON: read them if you like, edit them with the tools (they validate, keep
unknown keys and the undo history).

## The loop

1. `edit_animator_controller {path, create: true, ops: [...]}`: parameters, states, transitions,
   trees in one call. All or nothing: a failing op writes nothing and names `ops[i]`; an unknown field and a number
   out of range (`weight`, `offset`, `cycle_offset` 0..1; `duration`, `exit_time` 0 or more; an
   int `default` whole) are refused, never clamped.
2. Check `diagnostics` (the compiler's: unknown parameters, conditions of the wrong mode for the
   parameter's type, missing targets, unreachable states) and `warnings` (the validation: clips
   that do not load, dead ends, transitions never taken, unused parameters, names that are not
   Lua identifiers) in the result; `users` lists the Animators playing it.
3. Assign it: `add_component {entity, component: "animator", values: {animator: {controller}}}`.
4. `get_animator {entity}`: the rig (`bones`, `skins`, `humanoid`), the clips (`loaded`,
   `retargeted`, bound tracks) and any `bind_error`.
5. Try it in edit mode: `control_animator {entity, preview: "start", parameters: {Speed: 1},
   advance: 1}` evaluates it on the scene (`live`: per layer `state`, `normalized_time`, `next`,
   `transition_progress`); `preview: "stop"` puts the scene back. Or `play` and drive the running
   game with the same tool.

## Controller ops

| op | fields |
|---|---|
| `add_parameter` / `set_parameter` / `remove_parameter` / `rename_parameter` | `name`, `type` (float, int, bool, trigger), `default`, `new_name` (a rename follows into conditions, trees and state multipliers; a removal takes the conditions reading it and clears the trees and multipliers) |
| `add_layer` / `set_layer` / `remove_layer` | `name`, `weight`, `blending` (override, additive), `mask` (.mask path or null), `slot` (montage slot); the last layer stays |
| `add_state` / `set_state` / `remove_state` | `name`, `layer`, `machine` ("Combat/Inner"), `clip` or `blend_tree`, `position`, `speed`, `speed_parameter`, `time_parameter`, `cycle_offset`, `loop` (inherit, loop, once, ping_pong, clamp_forever, repeat), `tag`, `additive_reference` (first_frame, rest, time, clip: the .anim's own), `additive_time`, `mirror`, `foot_ik`, `retarget` (humanoid playback: left/right swapped, the feet to the clip's foot goals, retargeted even with the Animator's retarget off), `default: true`; a removal takes the transitions into it |
| `add_sub_state_machine` / `remove_sub_state_machine` | `name`, `layer`, `machine`, `position` |
| `add_transition` / `set_transition` / `remove_transition` | `from` (a state, `Any State`, `Entry`, `Exit:<machine>`), `to` (a state, a sub-state machine, `Exit`, `/State` from the layer root), `index`, `conditions [{parameter, mode, threshold}]`, `has_exit_time`, `exit_time`, `fixed_duration`, `duration`, `offset`, `interruption_source` (none, current, next, current_then_next, next_then_current), `ordered_interruption`, `can_transition_to_self`, `mute` |
| `set_default_state` | `name`, `layer`, `machine` |
| `set_blend_tree` | `state`, `blend_tree` |

Condition modes: `if` / `if_not` (bool, trigger), `greater` / `less` (float, int), `equals` /
`not_equal` (int). A trigger stays set until a transition uses it (then it resets).

A blend tree: `{type: "simple_1d", parameter: "Speed", children: [{clip, threshold}, ...]}`;
2D types take `parameter`, `parameter_y` and children `position: [x, y]`; `direct` children take
a `direct_parameter` (their weight); `time_scale` and `mirror` (flips the state's) per child;
a child may be a nested `blend_tree`. At most 32 children.

`from_player: <entity>` adds a state per clip of that GameObject's Animation Player (its default
clip the default state, its layers as layers with their weights, blends and masks): the upgrade
from a player to a controller. Creating a new file this way on a GameObject without an Animator
also adds one playing it and disables the player (it hands over), one scene undo step with the
file (the Inspector's "Create Animator Controller from Clips").

## Masks and montages

An Avatar Mask (`.mask`, version 2) gives every GameObject under the masked root a weight 0..1,
the first rule that applies: its `paths` entry (or the nearest ancestor's with `children: true`),
else (a humanoid rig) its body part, else `default`. Parts: `root` (root motion; with
`root_hierarchy` also the GameObject and what is above the hips), `body`, `head`, `left_arm` /
`right_arm` (shoulder to forearm), `left_hand` / `right_hand` (the hand bone), `left_fingers` /
`right_fingers` (below the hand), `left_leg` / `right_leg`; a part not set weighs `default`.
Animators' layers and Animation Player layers read the same masks.

```json
{"format": "jasperengine.mask", "version": 2, "default": 0,
 "body_parts": {"right_arm": 1, "right_hand": 1, "right_fingers": 0.5}, "root_hierarchy": false,
 "paths": [{"path": "mixamorig:Hips/mixamorig:Spine", "weight": 1, "children": true}]}
```

`edit_avatar_mask {path, create, default, body_parts: {head: 0.5, left_hand: false, ...: null},
root_hierarchy, paths: {"Hips/Spine": 0.5 | {weight, children} | null}, preview_model}`
(null unsets a part or removes a path); `from_model: <entity>` lists every path.
`get_animation {mask, root}` lists the weight and rule (`path`, `body_part:<part>`, `default`) of
what the root's player or Animator binds.

**Version 1 files** (S21's earlier masks and controllers, and upstream's anim-layers files, which
share the header) are read by their members and converted: upstream's flat `states` /
`transitions` nest into state machines, `humanoid` parts and `transforms` become `body_parts` and
`paths` (upstream's arm also weighs the hand), a state's `mirror` / `foot_ik` / `retarget` and a
child's `mirror` become the same S21 fields, members nobody reads are kept. Reads say
`read_as` (`s21_v1`, `upstream_v1`); the next save writes version 2 with every member kept.

`edit_montage {path, create, clip, slot, rate, blend_in {time, ease}, blend_out, sections [{name,
start, next}], events [{time, function, args}]}`: a one-shot clip on a layer whose `slot` matches
(Default: the base layer). Sections play in start order unless `next` says otherwise (`null`: the
montage ends there, itself: it loops). Play it with `control_animator {montage: {path, section}}`,
`stop_montage`, `montage_section {jump}` or `{from, next}`; Lua `Animator:playMontage(path)`.

## Lua

`GameObject:getAnimator()` (its Animation Player without one, nil without either):
`setFloat(name, value [, dampTime, deltaTime])`,
`setInteger`, `setBool`, `setTrigger`, `resetTrigger`, the getters, `play(state [, layer,
normalizedTime])`, `crossFade(state, duration [, layer, offset])`, `crossFadeInFixedTime`,
`getCurrentState(layer)` / `getNextState(layer)` (`name`, `fullPath`, `tag`, `normalizedTime`,
`length`, `loop`, `speed`), `isInTransition([layer])`, `setLayerWeight(layer, weight [,
fadeSeconds])`, upstream's names (`getCurrentStateName([layer])`, `getCurrentStateInfo` /
`getNextStateInfo` (a `ControllerStateInfo`: `isValid`, `name`, `fullName`, ...), `playInLayer`,
`crossFadeInLayer`, `hasController`, `hasParameter`), `playMontage`, `stopMontage`,
`montageJumpToSection`, `montageSetNextSection`, `setIKTarget(name, position)`, `setIKWeight`,
`setLookAt`, `isIKReached(name)`, `setIKSoften`, `setIKHingeAxis`, `setIKTwist`, `setIKAimOffset`,
`getBonePosition(slotOrPath)`, `getBoneRotation`, `hasBone`, `isHumanoid`,
`applyRootMotion`, `getDeltaPosition` / `getDeltaRotation`, and the callbacks `onEvent(event)`,
`onStateEnter(layer, state)`, `onStateExit(layer, state)` (the state's full path), `onMontageEnded(path, interrupted)`,
`onAnimatorMove(deltaPosition, deltaRotation)` (set it to take over root motion). Clip and montage
events also call the scripts' methods of the same name (`sendEventsToScripts`).

## Component settings

`speed`, `applyRootMotion` (the clips' root motion moves the GameObject, or its Character
Controller), `rootMotionBone`, `updateMode` (normal, animate_physics, unscaled_time),
`cullingMode` (always_animate, cull_update_transforms, cull_completely), `writeBones` (all,
exposed + `exposedBones`), `eventMode` (leader_only, all), `retarget` (auto, off), `clipOverrides`
([{original, replacement}]: the same controller with other clips) and `ik` (two_bone, fabrik,
look_at, foot_ik, foot_lock constraints: bones as paths or humanoid slots, a target GameObject or
Lua target, weight and an optional weight parameter; foot_ik raycasts the ground below each foot
and lowers the pelvis, foot_lock keeps a planted foot in place below a speed). Set a constraint's
fields with `set_properties` (`{"animator": {"ik[0].soften": 0.03}}`, one undo step). two_bone,
foot_ik and foot_lock take `soften` (0..1, the part of the reach that eases; 0: a hard limit),
`hingeAxis` (the middle bone's local axis, a positive turn straightens the joint; zero: auto) and
`twist` (degrees about the line to the target); look_at takes `aimOffset` (the eyes, from the last
bone). `get_animator`'s `live.ik[i].reached` is true when the last solve left the end within 1 mm
of its target (look_at: the aim within 0.1 degree, foot_ik: on ground it found), and false for a
partial weight, a soft limit, a clamp or a target out of reach. `control_animator`'s `ik` sets
`target` and `weight` on a running Animator, and in play mode also `soften`, `twist`, `hinge_axis`
and `aim_offset`. A controller holds no constraints, so `edit_animator_controller` does not take
them.

`live` also says how well IK and root motion work, from the last update. `ik[i]`: `weight` (the
solve's), `reach_error_mm`, foot_ik's `hit_point`, `hit_normal` (world) and `pelvis_offset_mm`, a
locked foot_lock's `lock_drift_mm` (from where it locked to where the clip has the foot: the slide
the lock hides). `root_motion`: `speed_mps`, `yaw_deg_s`, `accumulated_position` (metres since the
bind, in the rig frame of then) and `accumulated_yaw` (degrees), and `contact_source`: `foot_goals`
when the Animator has a foot_ik or foot_lock, with `slide_mm_s` (the planted foot's world speed)
and `foot_speed_mps` (its speed against the root: move at it for no slide), else `none` and no
slide. `set_scene_camera {display: {show_ik, show_root_motion, show_foot_trails}}` draws them in the
Scene view (the Inspector's Gizmos > Animation).
