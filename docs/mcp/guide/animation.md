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
   with the runtime sampler (`samples.tracks[i].values[n]` is the track's value at `times[n]`,
   the clip's wrap applied, or the wrap of `root`'s player entry: `samples.wrap_used`). A
   relative track gives its own offset / factor there, not the posed value (that is the preview).
4. Look at it: `preview_animation {animation: "Hero", action: "seek", time: 0.5}` poses the
   scene (edit mode, nothing saved), returns `bound` (the local transforms of up to 8 bound
   paths; `bound_total` counts them all) and `unresolved`; then `frame_entity` + `screenshot
   {view: "scene"}`. `action: "play"` plays it in the editor (every frame, looped whatever the
   wrap; `wait_ms` waits before answering). Always finish with `preview_animation {animation:
   "Hero", action: "stop"}` (restores the scene).
5. Run it: `play {action: "start"}` → `wait {game_seconds: 1}` → `get_runtime_entity` on a bound
   child (its Transform moved), `read_console` for event handler errors → `play {action: "stop"}`.

`get_animation {entity: "Hero"}` lists the player's states (`players[0].states: [{name, clip,
wrap}]`) and a binding report per state; use it after renames or when retargeting a clip.

## The .anim file

UTF-8 JSON, written canonically (fixed key order, 2-space indent, one key per line, tracks
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
- `preview.model` (asset-relative) is what the editor previews a clip on when nothing plays it;
  `source` records where an extracted, converted or baked clip came from. Unknown keys are kept.
- Writing the file yourself is fine (`write_file`, or your own tools: auto-sync reloads it and
  reports problems in `external_changes.problems` with line and JSON path); `write_file` refuses
  text that can't be read as a clip and returns the warnings. `check --anims` checks every clip
  from the CLI, and the entries of the scenes' and prefabs' players (prefab-instance overrides
  too): missing files, missing models, model clip names the model doesn't have. Prefer
  `edit_animation`: it validates paths against a root and is undoable.

## Binding paths and the root

- A path is the names from the root to the target joined by `/` (`"Hips/Spine/Chest"`); `""`
  is the root itself. `Name#N` picks the N-th later sibling with the same name (`"Wheel#1"` is
  the second "Wheel"). Names containing `/` or ending in `#<digits>` can't be addressed.
- The root is the GameObject with the Animation Player. For a model, paths are relative to the
  model's GameObject exactly as its node hierarchy is built (bones and mesh nodes by their glTF
  names, `mixamorig:Hips/mixamorig:Spine`). A model under a player always loads with its node
  hierarchy.
- `root` (edit_animation and get_animation) checks the paths against a real hierarchy and
  returns `bindings: {root, resolved, total, missing: [{path, channel, status, nearest,
  message}], missing_handlers}`. `nearest` lists existing paths by edit distance: a clip made
  for Hero on Robot reports `mixamorig:Spine` → nearest `Spine`. Without `root`, edit_animation
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
- With `root`, a track may give `target` (a GameObject ref) instead of `path`: it is turned into
  the path from the root (results always show `path`).

## Channels and keys

edit_animation keys are objects `{time, value, ease}` (lattice: `points` instead of `value`):

| channel | value | notes |
|---|---|---|
| `position`, `scale` | `[x, y, z]` local | `relative: true`: position adds to the rest (scene) value, scale multiplies it (`[1, 1, 1]` = unchanged) |
| `rotation` | `[x, y, z, w]`, `{"euler": [x, y, z]}` degrees or `{"quat": [...]}` | written as quaternions; with both `euler` and `quat` (a key read from a result) `euler` wins; `relative`: multiplies the rest rotation; a turn over 180 degrees between two keys needs middle keys |
| `morph` | `[w0, w1, ...]` one weight per morph target | shorter arrays mean 0 for the rest |
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
   spread as parameters.

```lua
function Hero:init()
    self.player = GameObject(self.scene, self.entity):getAnimationPlayer()
    self.player.onEvent = function(event)
        print(event.functionName, event.args[1])
    end
    self.player.onFinish = function(state) self.player:crossFade("Idle", 0.2) end
end

function Hero:onFootstep(side, strength)   -- called by the event above: ("left", 1)
end
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
  moves one state's weight; `queue(name[, fade])` plays after the current states finish (a
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
- Blending: channels are weighted and the remainder up to weight 1 is the rest pose (the scene
  value), so fading in from rest does not snap. Negative speeds play backwards.
- `Model:playAnimation(name[, fade])` and `stopAnimations()` use the model's Animation Player
  when it has one (else its entity animations, by the same names: the second of two glTF
  animations named "Wave" is "Wave 2").
- `preview_animation` drives the editor preview of a player's clip: `state` picks the clip
  (default: the default clip), `action` play / pause / seek / stop, `wait_ms` lets it play. The
  preview loops the clip whatever its wrap; Play mode follows the wrap.

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

Results: `{clip, created, length, wrap, frame_rate, tracks: [{index, path, channel, key_count,
keys, loop, relative}], events, changes, warnings, users, bindings?}`. `users` lists the
Animation Player entries in open scenes and loaded prefabs that play the file (`{scene, entity,
entry, state}`; closed scenes are not counted): the text says the edit changes all of them.

Undo: a clip edit (and create, extract) is ONE **project** undo step (`undo {scope:
"project"}`); the component that references the clip is a scene step. `create {from: <entity>}`
and `bake.clip` are ONE **scene** step (the file and the player together). The result text names
the scope.

## Model clips, converting and baking

- glTF/FBX animations are read-only clips named `<model>#<animation>`
  (`assets/models/hero.gltf#Walk`; `#<index>` for unnamed ones, `"Walk 2"` for a duplicate
  name). Reference them in `clips` like files; read them with `get_animation {clip}`
  (`get_asset_info` on the model lists the animations). To edit one, extract it:
  `edit_animation {clip: "assets/anim/walk.anim", create: {from: "assets/models/hero.gltf#Walk"}}`.
- An entity Animation converts with `create: {from: <the Animation GameObject>}`: keyframe tracks
  only (tweens, sprite animations, particles and nested animations stay in the Animation and are
  listed in `changes`). Its converted tracks stay in the Animation unless `create.remove_tracks:
  true`: when that Animation plays (autoplay, `Model:playAnimation`) it writes the same channels
  as the clip (a warning says so); remove the tracks, or the Animation (`delete_entities`).
- A pose animation bakes to a clip: `edit_pose_animation {path, bake: {model, clip: true}}`
  (`clip` may be an `.anim` path) writes the mapped bones' rotations and the hips' position and
  adds the clip to the model's Animation Player.
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

- Paths with the root's own name (`"Hero/Hips"` on Hero): paths start BELOW the root; `""` is
  the root.
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
- Forgetting `preview_animation stop`: the scene keeps showing the previewed pose until then
  (saves and Play restore it). `set_properties` during the preview writes the scene value (kept
  after the preview, also when equal to the previewed one).
