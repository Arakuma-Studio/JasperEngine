# Animation clips

An **Animation Clip** is a keyframe animation saved as a project asset: a `.anim` file (JSON), like
Unity's AnimationClip. Its tracks bind to GameObjects by **path** relative to the GameObject that
plays it (`""` is that GameObject, `Arm/Hand` a grandchild), not by scene ids. So one clip plays on
every hierarchy with the same child names: two prefabs, two scenes, or two characters of one rig.
An edit to the file changes every user, in the editor and in builds.

Clips are played by the **Animation Player** component (Unity's legacy Animation component): it
lists clips, plays them with fades, crossfades, blends and queues, applies wrap modes, and calls Lua
when a clip event fires. glTF and FBX animations can be used as read-only **model clips**
(`models/hero.gltf#Walk`) and extracted to editable `.anim` files. Entity Animations convert to
clips, and pose animations bake to them.

The older keyframe Animation (the **Animation** component: an entity sequence whose tracks target
scene objects, with tweens, sprite animations, particles and nested animations) is unchanged. Use it
for one-off scene choreography. Use clips for anything you reuse, and for characters.

AI agents edit the same files through the MCP tools `edit_animation`, `get_animation` and
`preview_animation` (see [docs/mcp/guide/animation.md](mcp/guide/animation.md)).

## Workflow

1. **Open the window.** Open **Window > Animation > Animation** (`Ctrl+6`), then select the GameObject to
   animate (a character, a door, a UI panel). If neither it nor any ancestor has an Animation Player,
   the window says "To begin animating *Name*, create an Animation Clip." and shows a **Create...**
   button.
2. **Create a clip.** **Create...** asks where to save the `.anim` (by default the folder of the
   model or prefab, else the assets folder). It adds an Animation Player listing the new clip, and
   nothing else. This is one scene undo step. You can also create clips in other ways:
   - **Create New Clip...** in the window's clip dropdown, or in the Inspector's Animation Player
     section, adds another clip to an existing player.
   - **Assets > Create > Animation Clip**, or **Create Animation Clip** in the Project window's
     menu, makes `New Animation.anim` in the current folder and starts renaming it. Drag it onto a
     GameObject in the Scene view or the Hierarchy: that adds an Animation Player (or reuses the one
     there) with the clip in one undo step.
   - Drop `.anim` files anywhere on the **Animation window**: they join the Animation Player of the
     GameObject the window shows (or of the selected one; a player is added when it has none), all in
     one scene undo step, and the first one opens as its state. A clip the player lists already is
     just opened. With nothing selected the clip opens on its own (you edit the file).
3. **Add properties.** **+ Add Property** opens a searchable tree of the bound GameObject's
   hierarchy. Pick **Position**, **Rotation** or **Scale** of any child, **Blend Shapes** of a mesh
   with morph targets, or **Lattice** of a lattice cage. The new row gets a key at the playhead
   that holds the current value. A row reads `Name : Channel` (for example `Hero : Position`;
   deeper paths start with `…/`, as in `…/mixamorig:Spine : Rotation`). Hover a row to see its
   full path, and open its foldout to see x/y/z, the blend shape names (that mesh's own: two
   meshes of a model can name their targets differently) or the lattice point count.
4. **Key it, one of two ways:**
   - **Record** (the red circle, or `R`): the timeline turns red (the curves view gets a red
     frame). Moving the bound GameObject or its children keys the clip at the playhead, on the
     nearest frame (a playing preview pauses there). This works with the Scene view's Move, Rotate
     and Scale tools (rotating or scaling a selection about its centre also keys the moved
     positions), the Inspector's Transform and Blend Shapes rows and its lattice point Offset, and
     dragging lattice points. One drag is one key edit.
     The Inspector tints the rows of animated properties red, and rows Record would start keying a
     fainter red. While Record is on the window keeps its clip whatever you select. Leaving Record
     goes back to how it was before: it stops the preview Record started (the scene is put back as it
     was, so recording never changes the scene), or keeps a preview you had started yourself.
   - **Without Record**: move the playhead, type values in the row's value column (rotations are
     Euler degrees), then press `K` to key every property at the playhead. `Shift+K` keys only the
     properties you changed. A typed value without Record is tinted and only previews, until you
     key it or move the playhead.
5. **Edit keys.** Click a key to select it, Shift-click to add keys, and Ctrl-click to toggle one.
   Drag a box to select several keys; on the summary row, the box covers every row. Drag the
   selected keys in time; they snap to frames. `Delete` removes them, `Ctrl+C` / `Ctrl+V` copy and
   paste them at the playhead onto the same properties, and `Ctrl+D` duplicates them. Right-click a
   key for **Add Key**, **Delete Keys**, **Interpolation** (Linear, Step and the ease presets, which
   set the segment that starts at the key), **Copy**, **Paste**, **Duplicate** and **Move Playhead
   Here**. Right-click a row label for **Retarget...** (or **Rebind...** on a missing row), **Add Key
   at Playhead** and **Remove Property**.
6. **Shape the motion in the Curves view.** The **Curves** button at the bottom left switches from
   the dopesheet to the curves (see [The curves view](#the-curves-view)).
7. **Add events.** The flag button adds an event at the playhead. You can also right-click the
   **Events** lane under the ruler and pick **Add Event**. Drag a flag to change its time. Click it
   to open the **Animation Event** editor: Time, Function (with a list of the methods the Lua
   scripts on the bound GameObject define), and up to 8 Args (Number, String or Bool). A flag whose
   function no script defines is drawn in yellow. See [Events](#events).
8. **Preview.** **Preview** (the timeline turns blue) shows the clip at the playhead in the Scene
   view. Press Play (`Space`) to loop it, and scrub the ruler. The preview never changes the scene:
   stopping it, saving the scene, pressing Play, switching scenes or closing the window puts every
   previewed value back. The scene is not marked modified.
9. **Play it in the game.** In the Inspector, the Animation Player's **Play Automatically** (on by
   default) plays the **Default Clip** when play mode or the game starts. From Lua, call
   `getAnimationPlayer()` and `play`, `crossFade`, `queue`... (see [Lua](#lua)).

The toolbar also has the playhead field with a **Frames** / **Seconds** choice, **Samples** (the
clip's frame rate, 1 to 240), **Snap keys to frames**, **Add Key**, **Add Event** and zoom. Samples
only sets the grid: keys are stored in seconds, and changing it moves no key. **Snap keys to
frames** moves keys onto the grid. If two keys snap to the same frame, they merge: the nearer one
stays, and a warning tells you.

### Undo

Clip edits (keys, properties, events, settings, Retarget) change the `.anim` file, so they are
**project** undo steps, not scene steps. `Ctrl/Cmd+Z` and `Ctrl/Cmd+Y` (and Edit > Undo / Redo) go
to the project history when your newest step is a clip edit made in the editor (the Animation
window, Record, the Inspector's `.anim` section, Extract to .anim... into a file alone, Assets >
Create > Animation Clip), whatever window has focus. Otherwise they undo the scene as usual. Edits made
by agents (MCP tools) keep their own undo scopes and do not change where the keys go. A drag of keys, or one
Record drag, is a single step. Actions that also change the scene are one **scene** step that
includes the new file: Create, Create New Clip..., Extract to .anim when the player switches to
the file, Convert to Clip and Bake to Animation Clip. Dropping clips on a GameObject or on the
window and Use Animation Player are scene steps too, with no file written. Undoing one of the first
group deletes the new file only if nobody edited it since. Otherwise the file is kept, and the console says so.

### The curves view

The left side lists the clip's properties. The eye button shows or hides a whole property, and each
coloured swatch shows or hides one component (x red, y green, z blue; blend shapes and lattice
points get their own colours). Click a property name to select its keys. The graph draws every
visible curve with the engine's own sampler, so what you see is what plays: eases, steps and the
tangents of imported clips. Rotations are shown as continuous Euler angles.

- Click, Shift-click, Ctrl-click and box select pick key points: one curve's point, not the whole
  key (keys selected in the dopesheet select all their visible points). Drag a point to move the
  selection in time (snapped to frames; a key moves in time as a whole) and in value: every selected
  point moves by the same amount on its own curve, also across properties and rotation keys
  (rotations in Euler degrees). Hold Shift to keep the drag on one axis. Clicking a property name
  selects its keys and frames its curves.
- Right-click a key or a curve segment to set the segment's **Interpolation**, add a key on that
  property, or delete keys.
- The wheel zooms the values at the mouse, `Ctrl`+wheel zooms time, `Shift`+wheel pans time, and the
  middle button pans both. `F` frames the selected keys (time and values), and `A` frames every
  visible curve.
- Selected keys with tangents (imported glTF CUBICSPLINE clips) show their stored tangent handles.
  The handles are read-only: free tangent editing is not part of this version.
- All curves share one value axis (rotations in degrees, positions in units, weights 0 to 1): hide
  curves or click a property name to frame the ones you work on. The axis shows `°` while only
  rotations are visible. Dense keys are drawn thinned; every key still selects and drags.

The window remembers the label column width and the Dopesheet/Curves choice in the editor layout.

## The Animation Player in the Inspector

**Add Component > Animation > Animation Player** (or Create / a drop, above). Its section shows:

- **Default Clip**: the state Play Automatically plays, and the one the Animation window opens
  first. "First Clip" means the first entry.
- **Clips**: the count, **+ Add Clip** (an empty entry; drop `.anim` files or a model on it), and
  **Create New Clip...**. Each entry is a row named after its state:
  - a file field with a picker and a clear button. Picking a model opens a list of its animations.
    A model clip shows "Read-Only", and a deleted file shows the file name in yellow with a
    warning icon. Right-click the field for Open in Animation Window, Extract to .anim... (model
    clips), Show in Project, Clear and Remove;
  - **Name**: the state name. The default is the file name without `.anim` (or the model clip's
    name). If two entries end up with the same name (two `walk.anim` files in different folders),
    the later one plays as "walk 2", "walk 3"... The saved list keeps what you typed. The row's
    label shows the name the state uses, and so does the hint of an empty Name field; when the
    player runs (play mode, a preview) the Console warns once per player which entries play under
    such a name;
  - **Wrap**: Default (the clip's own, for example "Default (Loop)"), Once, Loop, Ping Pong or Clamp
    Forever. Use it to loop a model clip, which itself plays once.
- **Play Automatically**, **Speed** (multiplies every state; negative plays backwards) and **Default
  Fade** (0.25 s: the crossfade time when none is given, between the 0.15 s of entity Animations and
  Unity's 0.3 s). The header checkbox disables the player: time stops and nothing is written.
- **Bindings**: `Bindings (all bound)`, or `Bindings ⚠ N problems` in yellow, in which case the
  foldout opens by itself. Each clip reports `n/m bound`. Missing paths show a "did you mean"
  suggestion, missing event handlers are listed, and a model in the legacy layout gets a **Reload
  Models in Node Hierarchy** button (see [Troubleshooting](#troubleshooting)).
- In play mode, **States**: for each state, whether it is playing or paused, a time bar
  (`0.91 / 1.00 s`) and its weight. Hover a state for its target weight, speed, wrap, whether it is
  queued, and its bound tracks.

Click a `.anim` file in the Project window and the Inspector shows the **clip asset**:
- **Open in Animation Window**;
- Settings: Frame Rate, Wrap Mode, Length (Auto = up to the last key or event, or a fixed length:
  longer holds the end pose, shorter cuts the tracks) and Preview Model (what the window previews
  the clip on when no GameObject is selected);
- Tracks (`path : Channel`, keys);
- Events (`0.50 s onWave("left", 1)`);
- Users (`N users in open scenes, M in loaded prefabs`; click one in the selected scene to select
  it);
- load warnings.

Edits there are project undo steps: `Ctrl/Cmd+Z` right after one undoes it, as in the Animation
window (see [Undo](#undo)). Double-clicking the file opens it in the Animation window.

## Events

An event (`time`, `function`, up to 8 number/string/bool `args`) fires when a playing state's time
crosses it: forward, backward, and across loop and ping-pong boundaries. It fires once per
crossing, even at low frame rates. Events fire for every state whose weight is above 0 and that is
not fading out. During a crossfade only the incoming clip fires, so you don't get double footsteps.
Every state of a `blend` fires. An event at time 0 fires when the state starts.

When an event fires, the engine calls, in order:

1. every `onEvent` subscriber of the player, with ONE `AnimationEvent` value: `event.functionName`,
   `event.args` (a 1-based table), `event.state` (the state name) and `event.time`;
2. the method with the event's function name on each enabled Lua script of the player's own
   GameObject (not its children), with the args as parameters: `onFootstep("left", 1)`.

Give events their own function names. An event named like a method the engine calls by itself
(`init`, `onDestroy`) or like an engine event a script registers (`onUpdate`, `onKeyDown`...) calls
that method again, with the event's args (Unity's `SendMessage` does the same). The binding report
(Inspector, Animation window, `get_animation`) warns about it.

Events and `onFinish` are dispatched after the frame's animation update, in time order. Player
calls made inside a handler take effect on the next frame. An event that nothing handles logs one
warning per clip and function in play mode. At edit time, the Bindings foldout and the window's
yellow flags show it. C++ scripts subscribe to the Animation Player's `onEvent(const
jasperengine::AnimationEvent&)` and `onFinish(const std::string& state)` with
`REGISTER_COMPONENT_EVENT` (the script editor's event list has an "Animation Player" entry). When
the GameObject has a C++ script, the edit-time handler check is skipped.

## Lua

```lua
---@class Hero : ScriptInstance
local Hero = { properties = {} }

function Hero:init()
    -- nil when the GameObject has no Animation Player (AnimationPlayer(scene, entity) adds one)
    self.player = GameObject(self.scene, self.entity):getAnimationPlayer()
    self.player.onEvent = function(event)
        print(event.functionName, event.state, event.args[1])
    end
    self.player.onFinish = function(state)
        if state == "Attack" then self.player:crossFade("Idle", 0.2) end
    end
    RegisterEngineEvent(self, "onKeyDown")
end

function Hero:onKeyDown(key)
    if key == Input.KEY_SPACE then
        self.player:crossFade("Attack", 0.1)
        self.player:queue("Idle", 0.2)          -- after Attack ends
    elseif key == Input.KEY_W then
        self.player:play("Walk")                 -- instant, the others stop
    end
end

function Hero:onFootstep(side, strength)    -- the clip's event {function: "onFootstep", args: ["left", 1]}
end

return Hero
```

- `play(name)` stops the other states (rewinding them) and plays `name` from 0 at weight 1.
  `play(name, fade)` with a fade above 0 is `crossFade(name, fade)`. `crossFade(name)` without a fade
  uses Default Fade.
- `blend(name, weight[, fade])` moves one state's weight and leaves the others alone. The weights
  of all playing states are combined. If they add up to less than 1, the rest is the scene's own
  value (the rest pose), so fading in from rest doesn't snap.
- `queue(name[, fade])` plays after the playing states finish. A `loop` or `ping_pong` state
  finishes at the end of its current cycle, and a `clamp_forever` one when it reaches its end. With
  a fade, the queued clip starts that many seconds early and crossfades. Queueing the playing clip
  restarts it at that point. Queueing with nothing playing plays at once. `play`, `crossFade` and
  `stop` clear the queue.
- `stop()` / `stop(name)` stop and rewind, keeping the pose. `rewind([name])` sets the time to 0.
  `sample()` applies the current times once, for example while paused. `isPlaying([name])`.
- **Wrap modes.** A `once` state that reaches its end calls `onFinish(state)`, stops advancing
  (`isPlaying` is false) and **holds its last pose** at its weight, until a fade or `stop` releases
  it. So `onFinish` followed by `crossFade("Idle")` blends from the end pose with no dip. With
  nothing else playing, the last pose stays. `play(name)` or `blend(name, w > 0)` of a held state
  restarts it; `blend(name, 0, fade)` fades it out. `clamp_forever` holds the last frame and keeps playing,
  calling `onFinish` once when it first reaches the end. `loop` and `ping_pong` never finish. A state
  started with a negative speed starts at its end.
- `getState(name)` returns an `AnimationState`. `time`, `normalizedTime`, `speed`, `weight`, `wrap`
  ("once", "loop", "ping_pong", "clamp_forever"; "default" goes back to the entry's) and `enabled`
  (false pauses it) are writable. `length`, `name` and `clip` are read-only. `isValid()` tells
  whether the state exists. A handle finds its state by name each time, so it survives clip list
  changes.
- `addClip(path[, name])` (asset-relative `.anim` or `model#clip`; false when the name is taken),
  `removeClip(name)`, `getClipCount()`, `getClipName(i)` (0-based); properties `speed`,
  `defaultClip`, `defaultFadeTime`, `playAutomatically`, `enabled`; `isLoaded()`.
- `Model:playAnimation(name[, fade])` uses the model's Animation Player first: it crossfades to that
  state (over Default Fade when no fade is given) and fades out the model's entity animations. An
  index addresses the player's states in list order (0-based); an index past them is the model's
  entity animation of that index. A name the player doesn't have falls back to the model's entity
  animation of that name. `stopAnimations(fade)` fades the player out too. Models
  without a player behave as before.

Players run in play mode and in the game. In the editor's edit mode, the Animation window's preview
shows clips. Calls made before the clips are loaded are kept and applied on load. Renaming or
reparenting a GameObject, in the editor or at runtime, rebinds the tracks on the next frame. A copy
made with `GameObject.instantiate` binds its own hierarchy. The full reference is in
[docs/mcp/guide/lua-api.md](mcp/guide/lua-api.md) (section "AnimationPlayer").

## Sharing a clip: paths

A track's path is the names from the player's GameObject down to the target, joined by `/`. `""` is
the player's own GameObject, and paths never start with its name. A model's glTF root node is a real
child, though, often named like the model: on the Hero model the spine is
`Hero/mixamorig:Hips/mixamorig:Spine`. `Wheel#1` is the second sibling named "Wheel" (`#0`, the
first, is never written). Names containing `/` or ending in `#` or `#<digits>` can't be addressed: Add Property shows them in yellow with the reason, and they can't be added.

Because paths are relative, a clip plays on any GameObject whose children have the same names:

- **Two prefabs** (a door and a gate with a child `Leaf`): give both an Animation Player listing
  `door_open.anim`. Edit the clip once and both change. The players are normal prefab properties,
  so a prefab instance can override the clip list or one entry's wrap.
- **Two scenes**: reference the same file from each.
- **Two characters of one rig** (same bone names): one walk clip plays on both. When objects of the
  rig were renamed or moved, the Bindings foldout lists the paths that don't resolve, each with the
  nearest existing path. **Retarget...** in the window's row menu renames a path in the clip. That
  changes the clip for every user, and the dialog says how many there are in open scenes. To keep
  the original, copy the file in the Project window first: the copy gets its own identity.
- **Another character rig** (other bone names and rest orientations): renaming paths doesn't make
  a clip fit it. Rotation keys are absolute local rotations, so the other rig's bones end up turned
  wrong (legs up, arms twisted). Make that character its own clip: bake a Pose Animation on it
  (**Bake to Animation Clip**), or key a new clip on it. For agents, the binding report offers no
  rename fix for such paths (`rest_mismatch`).

The files are referenced by GUID (the `guid` key the editor writes into each clip) plus the path.
Renaming or moving a clip keeps every Animation Player pointing at it: open scenes and loaded
prefabs right away, closed scenes and prefabs when they are opened (the scene opens as modified
with the repaired path). Deleting a clip leaves a yellow "Missing" entry, and undoing the delete
relinks it. A copied clip gets a new GUID.

## Model clips, Extract, and Use Animation Player

Every animation in a glTF/GLB or FBX model is a read-only clip named `<model>#<animation>`:
`models/hero.gltf#Walk`. Unnamed animations are `#Animation 2` (the glTF index). When two animations
share a name, the later one is `"Wave 2"`, `"Wave 3"`. Its paths are relative to the model's
GameObject and include the glTF scene's root nodes, as the Hierarchy shows them: bones and mesh
nodes by their glTF names, for example `Hero/mixamorig:Hips/mixamorig:Spine` (the hero's glTF root
node is named Hero too). It plays at 30 samples, wrap once (set the entry's Wrap to Loop to
loop it). Model clips can be referenced like files: pick a model in a clip field, or drop the model
on **+ Add Clip** and choose the animation.

- **Extract to .anim** (the Model section's row button, the clip field's menu, the clip asset view,
  or the window's Read-Only banner) writes an editable copy. The default name is `<animation>.anim`
  next to the model. When the model's Animation Player lists that model clip, its entries switch to
  the new file and keep their state names, all in one scene step. Otherwise extracting is a project
  step. The file remembers its source (`source`), but re-syncing from the model is not part of this
  version: extract again to pick up a changed model.
- **Add to Animation Player** (the film button on a model animation row) appends that model clip to
  the model's player, adding the player when there is none.
- **Use Animation Player** (Model section) switches the whole model in one scene step. It removes
  the model's entity animations and their tracks, sets the model to stop creating them on load, and
  adds an Animation Player listing every animation of the file. Afterwards the Model section says
  "Played by an Animation Player (no entity animations)". Scenes then store one short clip list
  instead of every key of every model instance.

Duplicate glTF names: the Model section labels the second entity animation row "Wave 2", matching
the model clip name. The entity animations themselves are both named "Wave", but without a player
`Model:playAnimation` and `Model:findAnimation` take the same names: "Wave" is the first one and
"Wave 2" the second. With a player, its states are "Wave" and "Wave 2".

## Converting an entity Animation, and baking a pose animation

**Convert to Clip...** (Inspector, on an Animation GameObject) turns the entity Animation's keyframe
tracks into a `.anim`:
- times are shifted by each frame's start time and track speed;
- targets become paths relative to the Animation's target (else the tracks' lowest common
  ancestor);
- Loop and Duration become the wrap and the length.

After you choose the file, a dialog says how many tracks convert, lists what doesn't convert
(tweens, sprite animations, particles, nested animations, and tracks outside the root, which stay in
the Animation) and asks whether to remove the converted track GameObjects (**Yes** / **No**;
**Cancel** stops). The file and an Animation Player playing it on the root are one scene step.
Unless you remove them, the converted tracks also stay in the entity Animation, which then writes
the same channels whenever it plays.

**Bake** in the Pose Animation window, and in the Inspector's Pose Animator section (it bakes the
animation the Pose Animator plays, under the file's name), opens a menu:
- **Bake to Animation Clip** writes `<name>.anim` next to the pose animation (the name comes from the
  field next to the button, by default the file name). The clip holds the mapped bones' rotations
  and the hips' position, and is listed in the character's Animation Player. Baking again rewrites
  the same clip and keeps its GUID. One scene step.
- **Bake into Scene** is the entity bake (an Animation of the character in this scene).

## Troubleshooting

- **Yellow rows, "(Missing!)", "N of M missing".** The path doesn't resolve on the bound GameObject:
  a renamed child, a different rig, or a clip made for another object. Hover the row for the message
  and its "did you mean". Then right-click it and choose **Rebind...** (it lists the nearest paths
  first, then every addressable path, with a search field) or **Remove Property**. Missing paths are
  not errors: at runtime the track is skipped, so a clip shared by two rigs may target bones only
  one has.
- **"Legacy model layout" / every bone missing.** The model was loaded without its node hierarchy:
  its bones are not GameObjects, so nothing binds. Adding an Animation Player normally reloads the
  model in the node hierarchy layout as part of the same step. A model that was saved in the old
  layout under a player, or moved under a player by duplicating or creating with a parent, stays
  legacy until reloaded. Click **Reload Models in Node Hierarchy** (window banner or Inspector
  Bindings). One undo step; tracks of entity Animations that targeted the
  old bones follow the rebuilt bones.
- **"several siblings share a name".** Two children with the same name: the path binds the first. Write
  `Name#1` for the second one, or rename one.
- **An event does nothing.** The handler must be a method on a Lua script of the player's own
  GameObject (not a child), named exactly like the event's function, or subscribe with `onEvent`.
  The callback gets one `event` value, not `(name, args)`. Methods a script inherits through a
  metatable count as found when the check can see the base table: `setmetatable(Hero, {__index =
  Base})`, `Hero = setmetatable({}, Base)`, with `Base` in the same script or
  `local Base = require("scripts.base")` (other ways of making classes are reported as not found,
  though they work at runtime).
- **A `once` clip doesn't snap back.** It holds its last pose (Unity does too). Use `stop`, crossfade
  to another state, or set the wrap to loop.
- **The preview loops a `once` clip.** The window's Play always loops. Play mode follows the wrap.
- **"This is a Unity .anim (YAML)".** Unity clips are not read. Create a JasperEngine clip (Assets >
  Create > Animation Clip) and key it, or import the animation in a glTF/FBX model and extract it.
- **"the file on disk is not a valid animation clip ... showing its last valid content".** A
  hand-edited file has a JSON error at that `line:col`. The window and the running players keep the
  last valid version until you fix it. `jasperengine-editor check --project . --anims` lists every
  clip problem from the command line.
- **A renamed model and closed scenes.** References to model clips (`hero.gltf#Walk`) are paths only,
  like the Model component's file. Renaming or moving the model updates open scenes and loaded
  prefabs, and the `preview` / `source` paths inside `.anim` files. Closed scenes and prefabs keep
  the old model path. Open them before you rename the model, or fix their clip fields afterwards.
- **Record keys only some of the selection.** Only GameObjects under the bound root are recorded.
  The others stay as they are, and the status line says how many (its tooltip says which root). A
  2D gizmo drag, a Rect Transform edit, instance or point moves stay normal scene edits (the status
  line says so the first time). MCP edits (`set_properties`, `set_transform`, `edit_lattice`) are
  never recorded, even while you are dragging something else.
- **An edit during the preview.** With the preview on and Record off, an edit (an Inspector row, a
  Blend Shapes slider, a lattice point) changes only what you edited; the previewed values of the
  other components or points are not saved into the scene. What you set on purpose is kept even
  when it equals the previewed value: an Inspector field, `set_properties` of a property, a lattice
  point set by index. A Scene view gizmo drag (or any whole-Transform write) that leaves a value as
  the preview shows it keeps the value from before the preview.
- **Blend shape weights after a reload.** A new model starts with its file's default weights.
  Weights you set (0 too) are saved with the scene or prefab and kept whenever the model reloads;
  loading another model file into the Model gives that file's defaults. If you re-export the model
  with more blend shapes, the new ones start at their defaults and the ones it had keep your weights.
  A scene or prefab saved by an older version keeps its weights up to the last non-zero one; the
  zero weights after it take the file's defaults on load, as they always did then.
- **Two animations write the same bones.** A model with both entity animations and an Animation
  Player has two writers. Use **Use Animation Player** to keep only the player, or call
  `Model:playAnimation`, which fades out the other side.

## The .anim file (for hand editing)

UTF-8 JSON written canonically: fixed key order, 2-space indent, one key per line, tracks sorted by
path then channel, events by time. Saving the same clip twice gives the same bytes, which makes
diffs readable. You can edit the file in any editor. The editor reloads it within a second (and
before every MCP call), and the open window, the Inspector and running players pick up the change.

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
    ]},
    {"path": "Body", "channel": "morph", "keys": [[0, [0, 0.5]], [1, [1, 0]]]},
    {"path": "Cage", "channel": "lattice", "keys": [
      [0, {"points": [[13, [0, 0, 0]]]}],
      [1, {"points": [[13, [0, 0.2, 0]]]}]
    ]}
  ],
  "events": [
    {"time": 0.5, "function": "onFootstep", "args": ["left", 1]}
  ],
  "preview": {"model": "models/hero.gltf"}
}
```

| key | meaning |
|---|---|
| `format`, `version` | required (`"jasperengine.anim"`, `1`). A missing or different format, a newer version, text that isn't JSON (reported with `line:col`) or a Unity YAML clip fail to load |
| `guid` | the clip's identity for references. The editor adds one to a clip without it. Don't copy it into another file: a copy made in the Project window gets its own |
| `frame_rate` | samples per second, 1 to 240 (default 30): the snapping grid only. Key times are seconds |
| `wrap` | `once` (default), `loop`, `ping_pong`, `clamp_forever` |
| `length` | optional, in seconds. Without it, the last key or event time. Longer holds the end pose, shorter cuts the tracks |
| `tracks[]` | `path`, `channel`, `keys`; optional `relative` (position, rotation, scale only) and `loop` (the track's keys repeat inside a longer clip) |
| `events[]` | `time`, `function` (a Lua identifier, not a keyword), `args` (at most 8 numbers, strings, booleans) |
| `preview.model` | editor only: the model or prefab (asset-relative) to preview on when no GameObject is bound |
| `source` | written by Extract, Convert and Bake (`{"kind": "model" / "entity" / "pose", "path", "name"}`); ignored at runtime |

Channels and values:

| channel | value |
|---|---|
| `position`, `scale` | `[x, y, z]`, local |
| `rotation` | a quaternion `[x, y, z, w]`, or `{"euler": [x, y, z]}` in degrees (read, written back as a quaternion). Between two Euler keys more than 180 degrees apart on an axis, add a middle key (the loader warns). Keys are written on the side of the key before them (the same rotation); a track with tangents keeps its signs, since its curve depends on them |
| `morph` | `[w0, w1, ...]`, one weight per morph target. Shorter arrays mean 0 for the rest; at most 8 targets |
| `lattice` | `{"points": [[flatIndex, [x, y, z]], ...]}` offsets of the cage points. A point a key omits holds 0 (warning). The path is the cage GameObject |

- A key is `[time, value]` or `[time, value, ease]`. `ease` is the easing of the segment that
  starts at that key: `linear` (default), `step`, or
  `<quad|cubic|quart|quint|sine|expo|circ|back|elastic|bounce>_<in|out|in_out>`. A `step` segment
  holds its first key's value; at exactly the next key's time the value is that key's own (Unity's
  rule, the same in entity Animations). The object form
  `{"time", "value", "ease", "in", "out"}` carries Hermite tangents (glTF CUBICSPLINE, value units
  per second). Tangents win over an ease, and a track in which only some keys have tangents gives
  the others linear ones (warning).
- `relative`: position adds to the target's rest (scene) value; rotation and scale multiply it, so
  `[1, 1, 1]` is "unchanged" for a relative scale.
- Everything but `format`/`version` is lenient. A bad key, track or event is dropped with a warning
  that carries its JSON path (`tracks[3].keys[2].value: expected [x, y, z]`), and the rest loads.
  Keys out of order are sorted, keys closer than a microsecond collapse (the later wins), and
  duplicate (path, channel) tracks merge. Unknown top-level keys are kept when the editor rewrites
  the file. A `"channel": "property"` track is kept but not played (reserved for a later version).
- The load warnings show in the clip asset Inspector, in `jasperengine-editor check --anims`, and to
  agents in `external_changes.problems`.

In a scene or prefab, an Animation Player is saved as:

```yaml
animationplayer:
  enabled: true
  clips:
    - {clip: anim/walk.anim, guid: 6f1d0c2a9b8e4f7da1c3e5b7092f4d6e, name: Walk, wrap: loop}
    - {clip: models/hero.gltf#Idle}
  defaultClip: Walk
  playAutomatically: true
  speed: 1
  defaultFadeTime: 0.25
```

`clip` paths are relative to the assets folder. `guid` is written for `.anim` files only (model
clips are path-only), and `name`/`wrap` only when set.

## Keyboard shortcuts

With the Animation window focused (and no text field active). These are the command catalog's
`animation.*` commands (context Animation): Edit > Shortcuts... rebinds them, and Help > Keyboard
Shortcuts lists them ([docs/editor/keyboard-shortcuts.md](editor/keyboard-shortcuts.md)):

| key | action | key | action |
|---|---|---|---|
| Space | play / pause the preview | Delete / Backspace | delete the selected keys (or the selected event) |
| K | key every property at the playhead | Ctrl/Cmd+C / V | copy keys / paste them at the playhead |
| Shift+K | key the properties you changed | Ctrl/Cmd+D | duplicate the selected keys |
| , / . | previous / next frame | F | frame the selection (Curves: values too) |
| Alt+, / Alt+. | previous / next key | A | frame all |
| R | Record on / off | Ctrl/Cmd+Z, Ctrl/Cmd+Y | undo / redo (clip edits: project history, see [Undo](#undo)) |

On macOS the `Ctrl` shortcuts are `Cmd` (as everywhere in the editor); `Control` with a key is not a
window shortcut there.

Mouse: `Ctrl`+wheel (or the wheel over the ruler) zooms time at the mouse, `Shift`+wheel and the
middle button pan, and dragging in the ruler scrubs. While the window has focus with a clip (or
its empty state) showing, Delete, Backspace, `Ctrl+D` and F2 act on the window, never on the
selected GameObject.

## Not in this version

Animator state machines and controllers, humanoid (muscle) clips, curves of other component
properties (light intensity, material colours: the format reserves `"channel": "property"`), root
motion, additive layers and avatar masks. Also: sprite-swap, active and colour/alpha curves (2D
games keep sprite animations and tweens), importer clip ranges and per-model-clip events, a
preview pane in the clip asset Inspector, event object parameters, free tangent editing, and
re-syncing an extracted clip from its model.
