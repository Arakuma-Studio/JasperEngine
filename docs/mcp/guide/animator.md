# Animator (`.controller`, `.mask`, `.montage`)

Unity's Mecanim on the engine's clips: an **Animator** component plays an **Animator Controller**
(`.controller`): float / int / bool / trigger parameters, layers (override or additive, an Avatar
Mask, a weight), a state machine per layer (states, sub-state machines, Entry / Any State / Exit,
transitions with conditions, exit times and interruption), blend trees (1D, 2D simple
directional, 2D freeform directional and cartesian, direct), montages on slots, an IK stack and
root motion. Its clips are the Animation Player's: `.anim` files and model clips
`assets/models/hero.gltf#Walk` (`jasper://docs/animation`). A humanoid clip made on another
humanoid model plays retargeted (the models' `.humanoid` maps, `jasper://docs/pose-animation`).

The Animation Player stays the simple path (a list of clips played by name); the Animator is the
state machine. Both can sit on one GameObject: the Animator owns the bones its clips animate and
runs after the player, so it wins where both write.

Tools (group `animation`): `get_animator` (read), `edit_animator_controller`, `edit_avatar_mask`,
`edit_montage` (each call one project undo step: `undo {scope: "project"}`), `control_animator`
(drive it: play mode or the edit preview) and `sample_animation_pose` (bone world transforms). The
files are canonical JSON: read them if you like, edit them with the tools (they validate, keep
unknown keys and the undo history).

## The loop

1. `edit_animator_controller {path, create: true, ops: [...]}`: parameters, states, transitions,
   trees in one call. All or nothing: a failing op writes nothing and names `ops[i]`.
2. Check `diagnostics` in the result (unknown parameters, conditions of the wrong mode for the
   parameter's type, missing targets, transitions that never fire, unreachable states).
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
| `add_parameter` / `set_parameter` / `remove_parameter` / `rename_parameter` | `name`, `type` (float, int, bool, trigger), `default`, `new_name` (a rename follows into conditions, trees and state multipliers) |
| `add_layer` / `set_layer` / `remove_layer` | `name`, `weight`, `blending` (override, additive), `mask` (.mask path or null), `slot` (montage slot) |
| `add_state` / `set_state` / `remove_state` | `name`, `layer`, `machine` ("Combat/Inner"), `clip` or `blend_tree`, `position`, `speed`, `speed_parameter`, `time_parameter`, `cycle_offset`, `loop` (inherit, loop, once), `tag`, `additive_reference` (first_frame, rest, time), `additive_time`, `default: true` |
| `add_sub_state_machine` / `remove_sub_state_machine` | `name`, `layer`, `machine`, `position` |
| `add_transition` / `set_transition` / `remove_transition` | `from` (a state, `Any State`, `Entry`, `Exit:<machine>`), `to` (a state, a sub-state machine, `Exit`, `/State` from the layer root), `index`, `conditions [{parameter, mode, threshold}]`, `has_exit_time`, `exit_time`, `fixed_duration`, `duration`, `offset`, `interruption_source` (none, current, next, current_then_next, next_then_current), `ordered_interruption`, `can_transition_to_self`, `mute` |
| `set_default_state` | `name`, `layer`, `machine` |
| `set_blend_tree` | `state`, `blend_tree` |

Condition modes: `if` / `if_not` (bool, trigger), `greater` / `less` (float, int), `equals` /
`not_equal` (int). A trigger stays set until a transition uses it (then it resets).

A blend tree: `{type: "simple_1d", parameter: "Speed", children: [{clip, threshold}, ...]}`;
2D types take `parameter`, `parameter_y` and children `position: [x, y]`; `direct` children take
a `direct_parameter` (their weight); `time_scale` per child; a child may be a nested
`blend_tree`. At most 32 children.

`from_player: <entity>` adds a state per clip of that GameObject's Animation Player (its default
clip the default state): the upgrade from a player to a controller.

## Masks and montages

`edit_avatar_mask {path, create, body_parts: {head: false, ...}, paths: {"Hero/Hips/Spine": 0.5,
"...": null}}`: humanoid body parts (root, body, head, left/right arm, hand, leg) and transform
paths below the Animator with weights (null removes one). `from_model: <entity>` lists every path.

`edit_montage {path, create, clip, slot, rate, blend_in {time, ease}, blend_out, sections [{name,
start, next}], events [{time, function, args}]}`: a one-shot clip on a layer whose `slot` matches
(Default: the base layer). Sections play in start order unless `next` says otherwise (`null`: the
montage ends there, itself: it loops). Play it with `control_animator {montage: {path, section}}`,
`stop_montage`, `montage_section {jump}` or `{from, next}`; Lua `Animator:playMontage(path)`.

## Lua

`GameObject:getAnimator()` (nil without one): `setFloat(name, value [, dampTime, deltaTime])`,
`setInteger`, `setBool`, `setTrigger`, `resetTrigger`, the getters, `play(state [, layer,
normalizedTime])`, `crossFade(state, duration [, layer, offset])`, `crossFadeInFixedTime`,
`getCurrentState(layer)` / `getNextState(layer)` (`name`, `fullPath`, `tag`, `normalizedTime`,
`length`, `loop`), `isInTransition`, `setLayerWeight`, `playMontage`, `stopMontage`,
`montageJumpToSection`, `montageSetNextSection`, `setIKTarget(name, position)`, `setIKWeight`,
`setLookAt`, `getBonePosition(slotOrPath)`, `getBoneRotation`, `hasBone`, `isHumanoid`,
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
and lowers the pelvis, foot_lock keeps a planted foot in place below a speed).
