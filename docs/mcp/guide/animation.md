# Animation clips and keyframe animations

Two ways to keyframe in JasperEngine, both edited with `edit_animation`:

- **Clips** (`.anim` files, Unity's AnimationClip): `edit_animation {clip: "assets/anim/bob.anim", ...}`.
  Tracks bind by **path** relative to the GameObject that plays the clip, so one file plays on
  every hierarchy with the same child names (two prefabs, two scenes, two characters of one
  rig). An **Animation Player** component (Unity's legacy Animation) on that GameObject lists the
  clips and plays them with fades, blends, queues, wrap modes and events. Editing the file
  changes every user. Prefer clips for anything reused, for characters and for gameplay code.
- **Entity Animations** (scene mode, no `clip`): an Animation GameObject in one scene whose
  tracks target scene objects by id, plus tweens, sprite animations, particles and nested
  animations. Use them for one-off scene choreography (UI intros, cutscene tweens).

Pose animations (`.poseanim`, `jasper://docs/pose-animation`) are a third, humanoid-only
format; `edit_pose_animation` can bake them into a clip (`bake: {model, clip: true}`).

## Quick start: the verification loop

1. Write the clip: `edit_animation {clip: "assets/anim/bob.anim", create: {}, wrap: "loop",
   tracks: [{path: "", channel: "position", keys: [{time: 0, value: [0, 0, 0]},
   {time: 0.5, value: [0, 1, 0], ease: "quad_out"}, {time: 1, value: [0, 0, 0]}]}]}`. Read
   `warnings` and `changes`. Pass `root` (the GameObject that will play it) to get `bindings`.
2. Make a GameObject play it: `add_component {entity: "Hero", component: "animationplayer",
   values: {animationplayer: {clips: [{clip: "assets/anim/bob.anim"}]}}}` (or `set_properties`
   on an existing player: `clips`, `clips[0].wrap`, `defaultClip`, `playAutomatically`, `speed`).
3. Read the numbers headless: `get_animation {clip, times: [0, 0.25, 0.5]}` samples every track
   with the runtime sampler (`samples.tracks[k].values[n]` is the value at `times[n]`; its
   `index` is the track's index in `tracks`; the clip's wrap applied, or the wrap of `root`'s
   player entry: `samples.wrap_used`). A relative track gives its own offset / factor there, not
   the posed value (that is the preview).
4. Look at it: `preview_animation {animation: "Hero", action: "seek", time: 0.5}` poses the
   scene (edit mode, nothing saved), returns `bound` (the local transforms of up to 8 bound
   paths, each with its `entity`; `bound_total` counts them all) and `unresolved`; then `frame_entity` + `screenshot
   {view: "scene"}`. `action: "play"` plays it in the editor (every frame, looped whatever the
   wrap; `wait_ms` waits before answering). Always finish with `preview_animation {animation:
   "Hero", action: "stop"}` (restores the scene).
5. Run it: `play {action: "start"}` → `wait {game_seconds: 1}` → `get_runtime_entity` on a bound
   child (its Transform moved), `read_console` for event handler errors → `play {action: "stop"}`.

`get_animation {entity: "Hero"}` lists the player's states (`players[0].states: [{name, clip,
wrap}]`) and a binding report per state; use it after renames or when retargeting a clip.

## The .anim file

UTF-8 JSON, written canonically (fixed key order, 2-space indent, one keyframe per line, tracks
sorted by path then channel, events by time). Writing the same data twice gives the same bytes.

```json
{
  "format": "jasperengine.anim",
  "version": 1,
  "guid": "6f1d0c2a9b8e4f7da1c3e5b7092f4d6e",
  "frame_rate": 30,
  "wrap": "loop",
  "length": 1,
  "tracks": [
    {"path": "", "channel": "position", "keys": [
      [0, [0, 0, 0]],
      [0.5, [0, 1, 0], "quad_out"],
      [1, [0, 0, 0]]
    ]},
    {"path": "Arm/Hand", "channel": "rotation", "relative": true, "keys": [
      [0, [0, 0, 0, 1]],
      [1, {"euler": [0, 90, 0]}]
    ]}
  ],
  "events": [{"time": 0.5, "function": "onFootstep", "args": ["left", 1]}],
  "preview": {"model": "models/hero.gltf"}
}
```

- `format` and `version` are required; everything else has a default. Not JSON, another
  format, a newer version or a Unity YAML `.anim` fail to load (JSON errors with `line:col`).
- Everything else is lenient: a bad key, track or event is dropped with a warning carrying its
  JSON path (`tracks[3].keys[2].value: expected [x, y, z]`) and the rest loads.
- `guid` identifies the clip for references (the editor writes one into a clip without it).
- `frame_rate` 1..240 (default 30) is the snapping grid only: keys are in seconds.
- `wrap`: `once` (default: plays to the end, holds the last pose), `loop`, `ping_pong`,
  `clamp_forever` (holds the last frame and keeps playing).
- `length` (seconds) is optional: default = the last key or event. Longer holds the end pose,
  shorter cuts the tracks. edit_animation `length: -1` returns to automatic.
- Keys in the file are `[time, value]` or `[time, value, ease]`; `{"time", "value", "ease",
  "in", "out"}` carries Hermite tangents (glTF CUBICSPLINE, units per second).
- `preview.model` (asset-relative) is what the Animation window previews a clip on when nothing
  plays it (`preview_animation` needs a GameObject whose player lists the clip);
  `source` records where an extracted, converted or baked clip came from. Unknown keys are kept.
- Writing the file yourself is fine (`write_file`, or your own tools: auto-sync reloads it and
  reports problems in `external_changes.problems` with line and JSON path); `write_file` refuses
  text that can't be read as a clip and returns the warnings. `check --anims` checks every clip
  from the CLI, and the entries of the scenes' and prefabs' players (prefab-instance overrides
  too): missing files, missing models, model clip names the model doesn't have. Prefer
  `edit_animation`: it validates paths against a root and is undoable.

## Binding paths and the root

- A path is the names from the root to the target joined by `/` (`"Body/Arm/Hand"`); `""`
  is the root itself. `Name#N` is the same-named sibling that has N earlier ones (`Wheel#1` = the
  second "Wheel"), as in entity refs. Names containing `/` or ending in `#` or `#<digits>` can't
  be addressed.
- The root is the GameObject with the Animation Player. For a model, paths are relative to the
  model's GameObject and include the glTF scene's root nodes, exactly as the Hierarchy shows them
  below it: the fixture hero's spine is `Hero/mixamorig:Hips/mixamorig:Spine` (its glTF root
  node is also named Hero). Read real paths from `get_animation {clip: "<model>#<animation>"}`
  (`tracks[].path`), from `get_scene_tree`, or pass `target` with `root`. A model under a player
  loads with its node hierarchy; one already loaded in the legacy layout is listed in
  `legacy_models`.
- `root` (edit_animation and get_animation) checks the paths against a real hierarchy and
  returns `bindings: {root, resolved, total, missing: [{path, channel, status, nearest,
  fix, message}], missing_handlers}`. `nearest` lists existing paths by edit distance: a clip
  made for Hero on Robot reports `Hero/mixamorig:Hips/mixamorig:Spine` → nearest
  `Robot/pelvis/spine` (another rig: see the fixes below). Without `root`, edit_animation
  reports against the first Animation Player that uses the clip.
- `status`: `missing_path`, `missing_component` (no Transform, no morph targets, no Lattice),
  `lattice_index_out_of_range`, `morph_index_out_of_range`, `legacy_model_layout`,
  `unaddressable_name`. Tracks bound to the first of several same-named siblings are listed in
  `ambiguous` (write `Name#N`). `legacy_models` lists models loaded without their node
  hierarchy (their bones can't bind): `add_component {entity: <root>, component:
  "animationplayer"}` adds the player, or with one already there reloads the models, its
  settings kept (one scene step, also with other components in the call: `reloaded`;
  `legacy_fix` says it).
- Missing paths are not errors: shared clips may target rigs that are not loaded. At runtime an
  unresolved track is skipped; renames and reparents rebind live.
- Path fixes are for renamed or reparented objects of the same rig. Rotation keys are absolute
  local rotations, so a clip made for another character rig (other bone names and rest
  orientations: Hero's walk on Robot) is mangled by renamed paths: give that character its own
  clip, e.g. bake its `.poseanim` on that model (`edit_pose_animation {path, bake: {model:
  "Robot", clip: true}}`, pose-animation guide) or key one on it.
- Fixing a missing path: rename the GameObject to match, or rename the track's path in the file.
  A `missing_path` with a nearest path carries `fix`, a `write_file` edit that renames it to
  `fix_path`: the first nearest path that is neither a track of the clip (same channel) nor
  another fix's target, so the fixes never merge two tracks and apply together (`fix_with` says
  so): `write_file {path: "<clip>", edits: [missing[0].fix, ...]}` keeps every key and tangent.
  Check each `fix_path` is the bone you mean. A nearest path whose name says the other side
  (Left/Right vs .L/.R) is skipped, and so is one whose rest rotation is more than 90 degrees from
  the clip's first rotation key of the path: no fix then, but `rest_mismatch` (the rest pose
  differs a lot, most likely another rig; `rig_mismatch` says what to do) or, when no free
  nearest path is left, `fix_conflict` (rename by hand). A same-rig clip whose first rotation key
  turns a bone (or a renamed prop) more than 90 degrees from its rest also gets `rest_mismatch`:
  renaming the path by hand (below) still works there. A file not in the canonical
  layout (written by hand) gets no fixes, and `fix_with` says how to rename instead. By hand: the
  canonical file has one track per `{"path": "<old>", "channel": "rotation"` line; or read the
  track (`get_animation {clip, max_keys: 100000}`), send its keys (with their `in`/`out`) under
  the new path and `keys: []` under the old one in one edit_animation call. Either changes the
  clip for every user (`users`); copy it first (`move_asset {copy: true}`) to keep the original.
- With `root`, a track may give `target` (a GameObject ref) instead of `path`: it is turned into
  the path from the root (results always show `path`).

## Channels and keys

edit_animation keys are objects `{time, value, ease}` (lattice: `points` instead of `value`):

| channel | value | notes |
|---|---|---|
| `position`, `scale` | `[x, y, z]` local | `relative: true`: position adds to the rest (scene) value, scale multiplies it (`[1, 1, 1]` = unchanged) |
| `rotation` | `[x, y, z, w]`, `{"euler": [x, y, z]}` degrees or `{"quat": [...]}` | written as quaternions; with both `euler` and `quat` (a key read from a result) `euler` wins; `relative`: multiplies the rest rotation; a turn over 180 degrees between two keys needs middle keys |
| `morph` | `[w0, w1, ...]` one weight per morph target (at most 8) | shorter arrays mean 0 for the rest |
| `lattice` | `points` (below) | the path is the cage (the lattice GameObject), not the mesh |

- `ease` is the easing of the segment that starts at the key: `linear` (default), `step`, or
  `<quad|cubic|quart|quint|sine|expo|circ|back|elastic|bounce>_<in|out|in_out>` (`quad_out`).
  A `step` segment holds its first key's value; at exactly the next key's time the value is that
  key's (Unity; entity Animations too).
- Lattice `points`: `{"<flat index>": [x, y, z]}` or `[[index, [x, y, z]], ...]` offsets;
  `[[[i, j, k], [x, y, z]], ...]` grid coordinates (index `i + rx*(j + ry*k)`), `"current"` (the
  cage's current offsets) and a pose name need the cage: pass `root`. `"rest"` holds every point
  the other keys animate at zero. A key that does not name a point interpolates it in time.
- Per track: `mode: "replace"` (default: the track's keys become these) or `"merge"` (keys at
  the same time within 1 ms are replaced, others added); `loop: true` repeats the track's own
  keys inside a longer clip; `keys: []` with mode replace removes the track.
- A track is identified by path + channel: giving the same pair edits that track. Its keys are
  `{path, target, channel, keys, mode, loop, relative}`; other keys are refused.
- Results return keys as `{time, value | points, ease}` (`ease` only when not linear, `in`/`out`
  for tangent keys of imported model clips), rotations as `{quat, euler}`; `max_keys` caps the
  keys listed per track (`key_count` is the real count).
- Tangents: a key you send with `in`/`out` (both, the value's width) keeps them, and a key sent
  back unchanged (same time and value, linear) keeps the tangents it had, in `replace` and
  `merge`; `merge` leaves the keys it does not touch as they are. A key you change without
  `in`/`out` in a track with tangents gets linear ones (the straight line to its neighbours; a
  warning says so). In a rotation track with tangents, such a key keeps the quaternion sign of
  the key it replaces (inserted: of the previous key), so `{"euler": ...}` values don't flip the
  curve. Lattice keys have no tangents.

## Events and Lua scripts

`events: [{time, function, args}]` (`args`: up to 8 numbers, strings or booleans) with
`events_mode: "replace"` (default: the list becomes these; `[]` clears) or `"merge"` (same time
and function replaced, others added). An event fires when a playing state's time crosses it
(forward, backward and across loops, once per crossing, even at low frame rates), for every
state with weight > 0 that is not fading out: during a crossfade only the incoming clip fires,
even at low weight, and every state of a `blend` fires. It calls, in order:

1. every `onEvent` subscriber of the player with ONE `AnimationEvent` value:
   `event.functionName`, `event.args` (a 1-based table), `event.state`, `event.time`;
2. the method named `function` on each Lua script of the player's GameObject, with the args
   spread as parameters. A `function` named like a lifecycle method (`init`, `onDestroy`) or an
   engine event (`onUpdate`...) calls that method again: `bindings.warnings` says so; use own names.

```lua
local Hero = {}

function Hero:init()
    self.player = GameObject(self.scene, self.entity):getAnimationPlayer()
    self.player.onEvent = function(event)
        print(event.functionName, event.args[1])
    end
    self.player.onFinish = function(state) self.player:crossFade("Idle", 0.2) end
end

function Hero:onFootstep(side, strength)   -- called by the event above: ("left", 1)
end

return Hero
```

An event with no subscriber and no method logs a warning once in play mode; `bindings`
(`missing_handlers`) reports it at edit time against the root's Lua scripts (methods inherited
through `setmetatable(T, {__index = Base})` / `setmetatable({}, Base)` count, with `Base` in the
script or from `require`).

## The Animation Player

Component `animationplayer` (Add Component > Animation > Animation Player). Properties:
`clips` (list of `{clip, name, wrap}`: `clip` an `.anim` path or a model clip
`"assets/models/hero.gltf#Walk"`, `name` the state name, default the file stem or animation
name, a later duplicate plays as "walk 2" (a warning once per player); `wrap` `default` (the
clip's), `once`, `loop`, `ping_pong`, `clamp_forever`),
`defaultClip` (state name; empty = the first), `playAutomatically` (true: the default clip plays
when the game starts), `speed` (multiplies every state), `defaultFadeTime` (0.25 s), `enabled`.

Playback (Unity legacy semantics), from Lua (`GameObject(...):getAnimationPlayer()`):

- `play(name)` stops the others and plays `name` from 0; `play(name, fade)` = `crossFade`.
- `crossFade(name[, fade])` fades the others out and `name` in; `blend(name, weight[, fade])`
  moves one state's weight (`blend(name, 0, fade)` fades it out); `queue(name[, fade])` plays
  after the current states finish (a
  loop / ping_pong state finishes at the end of its current cycle; queuing with nothing playing
  plays at once; `play`, `crossFade` and `stop` clear the queue).
- `stop()` / `stop(name)`, `rewind([name])`, `sample()` (apply the current times once),
  `isPlaying([name])`, `getState(name)` (`time`, `normalizedTime`, `speed`, `weight`, `wrap`,
  `enabled` writable; `length`, `clip`, `name`), `addClip(path[, name])`, `removeClip(name)`,
  `getClipCount()`, `getClipName(i)`, properties `speed`, `defaultClip`, `defaultFadeTime`,
  `playAutomatically`, `enabled`, `isLoaded()`, callbacks `onEvent`, `onFinish(state)`.
- `onFinish(state)` fires when a `once` state ends and when a `clamp_forever` state first
  reaches its end.
- A `once` state that reaches its end fires `onFinish`, stops advancing (`isPlaying` is false)
  and **holds its last pose** at its weight until a fade or `stop` releases it, so `onFinish →
  crossFade("Idle")` blends from the end pose. With nothing playing, the last pose stays.
  `play(name)` or `blend(name, w > 0)` of a held state restarts it.
- Blending: channels are weighted and the remainder up to weight 1 is the rest pose (the scene
  value), so fading in from rest does not snap. Negative speeds play backwards.
- `Model:playAnimation(name[, fade])` and `stopAnimations(fade)` use the model's Animation
  Player when it has one: a name is a player state first, else the entity animation of that
  name (the second of two glTF animations named "Wave" is "Wave 2"); an index is a player state
  (0-based, list order), past the player's states the entity animation of that index (without a
  player: the entity animations).
- `preview_animation` drives the editor preview of a player's clip: `state` picks the clip
  (default: the default clip), `action` play / pause / seek / stop, `wait_ms` lets it play. The
  preview loops the clip whatever its wrap; Play mode follows the wrap.

## Layers and masks

A player has layers (Unity's animation layers); layer 0 is the base. `set_properties
{animationplayer: {layers: [{name: "Base Layer"}, {name: "Holding", weight: 0, blend: "override",
mask: "assets/anim/arm.mask"}], "clips[1].layer": 1}}`. `layers[i].weight|blend|mask|name` set one
field (the base layer's weight and blend are refused: always 1, override; names are unique; a mask
must exist and parse). Each clip entry plays on `clips[i].layer` (default 0).
- Override replaces what the layers below produce, as much as `weight * mask`; Additive adds the
  clip's change from its `additive_reference` time (default its first frame). A layer without a
  mask animates only the channels its clips key: the rest passes through from below.
- Lua: `player:setLayerWeight("Holding", w[, fade])`, `getLayerWeight`, `setLayerMask`,
  `setLayerBlendMode`, `stopLayer`, `play(name, fade, PlayMode.STOP_ALL)`; `play` / `crossFade` /
  `queue` act on the target's layer only. Events fire only on layers with weight > 0.

An Avatar Mask is a `.mask` JSON file; write it with `write_file` (it refuses unreadable text
with `line:col`, Unity YAML too, and returns the lenient reader's warnings):

```json
{"format": "jasperengine.mask", "version": 1, "default": 0,
 "humanoid": {"right_arm": 1, "right_fingers": 1},
 "transforms": [{"path": "Granjero/mixamorig:Hips/mixamorig:Spine/mixamorig:Spine1/mixamorig:Spine2/mixamorig:RightShoulder/Can", "weight": 1, "children": true}]}
```

- `default`: the weight of what no rule covers (1 if absent: an empty mask passes all).
- `transforms[]`: clip-grammar paths from the player's GameObject; the entry of a bone, else its
  nearest ancestor's with `children: true`, wins.
- `humanoid`: `root`, `body`, `head`, `left_leg`, `right_leg`, `left_arm`, `right_arm`,
  `left_fingers`, `right_fingers` (0..1 or true/false; unlisted parts take `default`); used when
  the GameObject has a humanoid rig (Mixamo, VRM... names), checked after the transforms.
- Check it: `get_animation {mask, root}` lists `coverage: [{path, weight, rule: transform |
  humanoid:<part> | default}]` for the entities the root's player binds (200 at most,
  `coverage_total`), `humanoid_rig` false when the body parts can't apply, and `users`.
  `get_animation {entity}` has `players[0].layers` (`live_weight` and `queue` in play) and
  `mask_coverage: [{layer, covered, total, warnings}]`.
- Preview: `preview_animation {animation, state: "Hold", over: "Walk"}` shows a layer's state over a
  base state with its mask, as in the game. Verify poses numerically in play with
  `get_runtime_entity` on the bones (the right forearm holds, the left one walks).

## Animator Controllers

A `.controller` (Unity's Animator Controller) owns a player's states when set:
`set_properties {animationplayer: {controller: "assets/anim/hero.controller"}}` (the clip list and
layers are then ignored). Build it with `edit_animator {controller, create?, ops}`: all ops apply
or none, one undo step; the result is the summary (indices, defaults) with `warnings` (clips that
do not load, missing or mistyped parameters, dead ends, unreachable states, transitions that fire
at once, unused parameters) and `users`. `create: {}` starts with a Base Layer;
`create: {from_entity}` makes one state per clip entry of that player (its layers too) and sets it
as the player's controller (a scene step). Read one: `get_animation {controller}`.

Ops (`layer` is an index or a name, default 0; states are named per layer):
- `add_parameter {name, type: float|int|bool|trigger, default}`, `update_parameter {name, rename?,
  type?, default?}` (a rename follows into conditions, blend trees, speed parameters),
  `remove_parameter {name}` (its conditions go).
- `add_layer {name, weight, blend, mask}`, `update_layer {layer, name?, weight?, blend?, mask?}`,
  `remove_layer {layer}`, `move_layer {layer, to}` (the base layer stays first).
- `add_state {layer, name, clip | blend_tree, speed, speed_parameter, wrap, cycle_offset,
  position}` (neither clip nor tree: an empty state; positions default to a grid);
  `blend_tree: {parameter, children: [{clip, threshold, speed?}], automate_thresholds?}` (1D;
  children blend linearly between neighbouring thresholds and play in step; only the heaviest
  fires events). `update_state {layer, state, rename?, ...}`, `remove_state {layer, state}` (its
  transitions go), `set_default_state {layer, state}`.
- `add_transition {layer, from (a state or "any"), to, conditions: [{parameter, mode, threshold}],
  has_exit_time (true), exit_time (0.75, normalized), duration (0.25), fixed_duration (true:
  seconds), offset (0), interruption (none|current_state|next_state), can_transition_to_self
  (Any State only, true)}`; `update_transition {layer, index, ...}`, `remove_transition {layer,
  index}` (indices from the summary; list order is priority). Modes: `greater`/`less` (float,
  int), `equals`/`not_equal` (int), `if`/`if_not` (bool; a trigger uses `if`).

Rules (Unity's): each update a layer checks its Any State transitions, then the current state's,
and takes the first whose conditions hold and (with exit time) whose source crossed `exit_time`;
the triggers it read reset. A transition blends linearly over its duration; while it runs only its
interruption source can start another. On the first update every layer enters its default state.
Errors name the op and field: `ops[2].to: no state "Run" in layer "Base Layer" (states: ...)`.

Example, the Garden farmer (two layers, a float):

```json
{"controller": "assets/anim/granjero.controller", "create": {}, "ops": [
 {"op": "add_parameter", "name": "Speed", "type": "float"},
 {"op": "add_state", "name": "Idle", "clip": "assets/models/granjero.glb#Happy Idle", "wrap": "loop"},
 {"op": "add_state", "name": "Running", "clip": "assets/models/granjero.glb#Running", "wrap": "loop"},
 {"op": "add_transition", "from": "Idle", "to": "Running", "has_exit_time": false, "duration": 0.15,
  "conditions": [{"parameter": "Speed", "mode": "greater", "threshold": 0.01}]},
 {"op": "add_transition", "from": "Running", "to": "Idle", "has_exit_time": false, "duration": 0.15,
  "conditions": [{"parameter": "Speed", "mode": "less", "threshold": 0.01}]},
 {"op": "add_layer", "name": "Holding", "weight": 0, "mask": "assets/anim/arm.mask"},
 {"op": "add_state", "layer": "Holding", "name": "Hold", "clip": "assets/anim/holding.anim"}]}
```

Drive it in play: `eval_lua "GameObject(scene, id):getAnimator():setFloat('Speed', 2)"`, then
`get_animation {entity}`: `players[0].controller` has `parameters` and per layer `current`,
`normalized_time` and `transition: {to, progress}`. Lua also has `setBool`, `setInteger`,
`setTrigger`, `resetTrigger`, `getCurrentStateInfo(layer)`, `isInTransition`,
`playInLayer(state, layer, normalizedTime)`, `crossFadeInLayer(state, seconds, layer)`, and
per-object script methods `onStateEnter(layer, name)` / `onStateExit(layer, name)`. A type
mismatch (`setBool` on a trigger) warns once and converts. Parameter names are Lua identifiers.

## edit_animation: clip mode and scene mode

`clip` selects the mode. Each mode refuses the other's arguments with a fix-it message:

| clip mode refuses | use instead |
|---|---|
| `loop`, `duration` | `wrap`, `length` |
| `tweens`, `add_actions`, `set_frames`, `remove_actions` | scene actions: edit_animation without `clip`; remove a clip track with `keys: []` |
| `target`, `animation`, `name` | `tracks[].path` (+ `root`), the player entry's `name` |
| `autoplay`, `speed` | the Animation Player's `playAutomatically`, `speed` (set_properties) |
| `tracks[].start_time`, `tracks[].name` | shift the key times; tracks are named by path + channel |

| scene mode refuses | why |
|---|---|
| `wrap`, `length`, `frame_rate`, `events`, `events_mode`, `root`, `create` | clip-file settings: pass `clip` |
| `tracks[].path` | clip binding: pass `clip`; scene tracks use `target` |

Clip mode arguments: `clip`, `create` (`{}` a new file; `{from: <entity Animation>}` converts
its keyframe tracks into the file and gives the Animation's target, or `root`, an Animation
Player playing it, and `remove_tracks: true` also removes the converted tracks from the
Animation; `{from: "assets/models/hero.gltf#Walk"}` extracts a model clip into an editable
file), `tracks`, `events`, `events_mode`, `wrap`, `frame_rate`, `length`, `root`, `scene` (of
`root` / `from`), `max_keys`. `create` fails when the file exists; without it the file must
exist. A call is all or nothing; a setting given with its current value is a no-change, not an
error (retries are safe).

Results: `{clip, created, read_only, length, explicit_length, wrap, frame_rate, tracks: [{index,
path, channel, key_count, keys, loop, relative, points?}], events, preview_model?, source?,
changes, warnings, users, users_total?, bindings?}`. `users` lists the Animation Player entries
in open scenes and loaded prefabs that play the file (`{scene, entity, entry, state}`; a prefab
user is `{prefab, entry}`; at most 50, `users_total` counts them all; closed scenes are not
counted): the text says the edit changes all of them. `create {from: <entity>}` adds `player`,
`added_player`, `removed_tracks`; an extract adds `model_clip_users` (below).

Undo: a clip edit (and create, extract) is ONE **project** undo step (`undo {scope:
"project"}`); the component that references the clip is a scene step. `create {from: <entity>}`,
an extract with `root` and `bake.clip` are ONE **scene** step (the file and the player together).
The result text names the scope. Undoing a `create {from}` deletes the file only if no later edit
changed it (else the file stays and the console says so).

## Model clips, converting and baking

- glTF/FBX animations are read-only clips named `<model>#<animation>`
  (`assets/models/hero.gltf#Walk`; `#Animation <i>` for unnamed ones (`#<i>` is accepted too),
  `"Walk 2"` for a duplicate name). Reference them in `clips` like files; read them with
  `get_animation {clip}` (`get_asset_info` on the model lists the animations). To edit one,
  extract it: `edit_animation {clip: "assets/anim/walk.anim", create: {from:
  "assets/models/hero.gltf#Walk"}, root: "Hero"}`. With `root` whose Animation Player lists the
  model clip, those entries switch to the new file (state names kept) in the same call, one
  scene step, like the Animation window's Extract. Without it the file alone is written (a
  project step) and the players keep playing the model clip, so edits of the file don't show
  there: `model_clip_users` lists them; point one at the copy with `set_properties
  {animationplayer: {"clips[i].clip": "assets/anim/walk.anim"}}` (`get_animation {entity}` shows
  i).
- An entity Animation converts with `create: {from: <the Animation GameObject>}`: keyframe tracks
  only (tweens, sprite animations, particles and nested animations stay in the Animation and are
  listed in `changes`). Its converted tracks stay in the Animation unless `create.remove_tracks:
  true`: when that Animation plays (autoplay, `Model:playAnimation`) it writes the same channels
  as the clip (a warning says so); remove the tracks, or the Animation (`delete_entities`).
- A pose animation bakes to a clip: `edit_pose_animation {path, bake: {model, clip: true}}`
  writes the mapped bones' rotations and the hips' position and adds the clip to the model's
  Animation Player. `true` writes `<name>.anim` next to the `.poseanim`; `clip` may be an `.anim`
  path instead. Baking again rewrites the clip it baked before (GUID kept); another file of that
  name makes it take a free one (`walk 2.anim`), and an explicit path to an existing file not baked from this pose animation is refused. The
  result's `bake.tracks` is a count (as in the entity bake).
- No tool does the Inspector's **Use Animation Player** (entity animations replaced by the
  player): a model with entity animations and a player has two writers; `Model:playAnimation`
  fades the entity ones out when it plays a player state.
- `list_assets {type: "animation"}` lists `.anim` and `.poseanim` files; `get_asset_info` on an
  `.anim` shows its settings, tracks, events, warnings and users; `move_asset` keeps the players
  pointing at a moved clip (GUID), `delete_asset` leaves them a Missing reference (undo relinks).

## Scene mode: entity Animations and tweens

Without `clip`, edit_animation edits an Animation GameObject (`animation`, or `target` to create
one): `tracks` as above but with `target` (a GameObject; default `target`) instead of `path` and
an optional `start_time`; `tweens: [{type, from, to, duration, ease, loop, ping_pong, ...}]`:
`type` position, rotation, scale, color or alpha; `from` defaults to the target's current value;
`ping_pong` goes there and back in twice the duration; a rotation tween with `spin: true` turns
`from` by `angle` degrees (default 360) around `axis` (default `[0, 1, 0]`) instead of reaching
`to` (for turns over 180 degrees, instead of middle keys); a track's `loop` starts it over after
its last key; a `relative` track there offsets from the target's pose when it starts (an
anchored UI element moves by its layout offset). `add_actions` nests existing actions;
`set_frames` / `remove_actions` use the indices before the call; `loop`, `duration` (-1: until
the actions end), `autoplay`, `speed` set the Animation. One scene undo step per call.

## Common mistakes

- Paths that repeat the root GameObject's own name: paths start below the root (`""` is the
  root). A model's glTF root node is a real child, often named like the model, so
  `Hero/mixamorig:Hips` on the Hero model is correct: trust `bindings.nearest`.
- `undo` without `scope` undoes the scene, not a clip edit (`undo {scope: "project"}`).
- A lattice track on the mesh: the path is the cage GameObject.
- Keys only at 0 and 180 degrees: the rotation takes the short way or flips; add a middle key
  (entity Animations: or a rotation tween with `spin`).
- Relative scale keys of `[0, 0, 0]` for "unchanged": relative scale multiplies, use `[1, 1, 1]`.
- Editing a key read from a result: change `euler` (it wins over `quat`), or send only one.
- Expecting `once` clips to snap back: they hold the last pose (use `stop`, or `loop`).
- Editing a model clip: it is read-only; extract it first.
- Events on a script of a child: handlers run on the scripts of the player's own GameObject.
- `onEvent = function(name, args)`: the callback gets one `event` (`event.functionName`,
  `event.args`).
- A layer at weight 0 shows nothing (and fires no events): raise it with `setLayerWeight` or
  `layers[i].weight`; a layer whose mask is missing or broken contributes nothing (the Console says why).
- Setting `clips` on a player that has a `controller`: the controller owns the states; edit it with
  `edit_animator`.
- Forgetting `preview_animation stop`: the scene keeps showing the previewed pose until then
  (saves and Play restore it). `set_properties` during the preview writes the scene value (kept
  after the preview, also when equal to the previewed one).
