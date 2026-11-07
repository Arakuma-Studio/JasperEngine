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
   with morph targets, or **Lattice** of a lattice cage. Under them come the GameObject's
   [component properties](#property-tracks): **GameObject ▸ Active**, then one group per component
   with its Inspector name (Image, Light, Camera, Rect Transform, Audio Source, Canvas Group,
   Button...) holding **Enabled** and what it can animate ("Color", "Intensity", "Y FOV",
   "Anchored Position"...), a mesh's **Material [N]** values, a sprite's **Sprite Renderer ▸
   Color** and its frame, and each Lua script's declared fields (**Torch (Script) ▸ glow**). A
   colour also offers **Alpha only**. A property already in the clip is greyed out. The new row
   gets a key at the playhead that holds the current value. A row reads `Name : Channel` (for
   example `Hero : Position`, `Panel : Image.Color`; deeper paths start with `…/`, as in
   `…/mixamorig:Spine : Rotation`). Hover a row to see its full path, and open its foldout to see
   x/y/z, a colour's r/g/b/a, the blend shape names (that mesh's own: two meshes of a model can
   name their targets differently) or the lattice point count.
4. **Key it, one of two ways:**
   - **Record** (the red circle, or `R`): the timeline turns red (the curves view gets a red
     frame). Moving the bound GameObject or its children keys the clip at the playhead, on the
     nearest frame (a playing preview pauses there). This works with the Scene view's Move, Rotate
     and Scale tools (rotating or scaling a selection about its centre also keys the moved
     positions), the Inspector's Transform and Blend Shapes rows and its lattice point Offset,
     dragging lattice points, and every Inspector row of an animatable component property (a
     colour picker, Intensity, a Lua script's fields...), the GameObject's active checkbox and the
     components' enable checkboxes (the edit is keyed and previewed, it is not a scene edit). One
     drag is one key edit.
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
- **Tangents** (Unity's key tangents, on every scalar curve: position, scale, blend shapes,
  lattice offsets and continuous properties). Right-click a key for **Auto** (Clamped Auto: smooth,
  flat at a peak, never overshooting its neighbours, and computed again when the key or a
  neighbour moves), **Free Smooth**, **Flat**, **Broken**, and **Left / Right / Both Tangents ▸
  Free / Linear / Constant** (Constant right holds the key's value until the next key; left, the
  previous key's). Selected keys show their handles: drag one to set that side's slope (the other
  side follows unless the key is Broken); each drag is one undo step. Giving tangents to one key
  of a track gives its other keys their linear slopes, so the rest of the curve does not change
  where it was linear: tangents win over eases, so the track's other eases (`cubic_in_out`,
  `bounce`...) become linear tangents, and the window says how many did (steps stay: Constant).
  Rotations keep the Interpolation presets; their handles (imported glTF CUBICSPLINE tangents)
  are read-only.
- Stepped properties (bools, ints, sprite frames) draw as steps and move in time only.
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

## Property tracks

A property track animates a component value of a GameObject under the root, as Unity's clips
animate `m_IsActive`, `m_Enabled`, `m_Color`, material and MonoBehaviour fields. It is named by the
path, the component (its Catalog key, the one `set_properties` uses, or `gameobject` /
`script:<Class>`) and the property (the Catalog name, optionally with one component: `.r .g .b .a`
on colours, `.x .y .z .w` on vectors). Keys use the Inspector's units: colours in sRGB (as the
colour pickers show them), spot cones in degrees (the full angle, as the Inspector shows it),
everything else as stored. Values interpolate in those units (Unity's colour curves do too).

| component | properties | Unity |
|---|---|---|
| `gameobject` | `active` (bool) | `m_IsActive` |
| any component with the enable checkbox (`ui`, `mesh`, `sprite`, `light`, `camera`, `audiosource`, `canvasgroup`, `rigidbody`, `onscreenstick`...) | `enabled` (bool), applied as the Lua setter does: a Rigidbody 2D leaves or joins the world at once, a 3D body on the physics step; disabling a Trail Renderer clears its points; enabling a broken Joint 2D makes it a fresh joint | `m_Enabled` |
| `ui` (Image, Text, Button graphic, any UI element) | `color` (Color4), `shaderUniforms.<member>` | `m_Color`, TMP `m_fontColor` |
| `button` | `colorNormal`, `colorHovered`, `colorPressed`, `colorDisabled`, `colorSelected` (Color4), `disabled` (bool) | `m_Colors.*` |
| `canvasgroup` | `alpha` (float, 0 to 1), `interactable`, `blocksRaycasts`, `ignoreParentGroups` (bool) | `m_Alpha`... |
| `recttransform` | `positionOffset` (Vector2, the anchored position), `width`, `height`, `anchorOffsetLeft/Top/Right/Bottom` (ints that interpolate and round), `pivot` (Vector2), `anchorPointLeft/Top/Right/Bottom` (float) | `m_AnchoredPosition`, `m_SizeDelta`, `m_Pivot`, `m_AnchorMin/Max` |
| `mesh` (meshes, sprites, model meshes, tilemaps) | `submeshes[N].material.baseColorFactor` (Color4; a sprite's colour is submesh 0's), `.emissiveFactor` (Color3), `.metallicFactor`, `.roughnessFactor`, `.alphaCutoff` (float), `submeshes[N].textureRect` (offset x y, scale x y), `submeshes[N].material.shaderParams.<member>` | `material._Color`... |
| `sprite` | `frame` (the frame index, stepped) | `m_Sprite` (sprite swap) |
| `light` | `color` (Color3), `intensity`, `range` (float), `innerConeCos`, `outerConeCos` (keyed as the full cone angle in degrees, Unity's Spot Angle: not the cosine, not the half angle) | `m_Intensity`, `m_SpotAngle` |
| `light2d` | `color`, `intensity`, `range`, `falloff`, `height` | |
| `camera` | `yfov` (radians), `near`, `far`, `left`, `right`, `bottom`, `top` | field of view, clip planes, orthographic size |
| `audiosource` | `volume`, `speed` (pitch), `pan` | `m_Volume`, `m_Pitch` |
| `fog` | `color`, `density`, `linearStart`, `linearEnd` | |
| `skybox` | `color` (Color4) | |
| `progressbar` | `value` | |
| `linerenderer`, `points`, `skybox` | `shaderUniforms.<member>` | |
| `script:<Class>` | a declared Lua `properties` field of type bool, int, float, vector2/3/4 or color3/4, or `enabled` (the script's checkbox: onEnable / onDisable) | MonoBehaviour fields |

```json
{"path": "Panel/Stats/puntaje", "channel": "property", "component": "gameobject", "property": "active",
 "keys": [[0, false], [0.5, true]]},
{"path": "Fade", "channel": "property", "component": "ui", "property": "color.a",
 "keys": [{"time": 0, "value": 0, "in": 0, "out": 0, "tangent_mode": "auto"}, {"time": 1.5, "value": 1, "in": 0, "out": 0, "tangent_mode": "auto"}]},
{"path": "", "channel": "property", "component": "canvasgroup", "property": "alpha", "keys": [[0, 0], [0.333, 1]]},
{"path": "", "channel": "property", "component": "script:Torch", "property": "glow", "keys": [[0, 0.2], [0.5, 1, "sine_in_out"]]},
{"path": "Ship", "channel": "property", "component": "sprite", "property": "frame", "keys": [[0, 0], [0.083, 1], [0.166, 2]]}
```

- **Stepped and smooth values.** Bools, ints and sprite frames step: the value is the key at or
  before the time (at a key's time, that key's value); an ease on them is ignored with a warning.
  Everything else uses the eases and tangents. In a blend or crossfade a smooth value is the
  weighted average (the scene value fills what the states leave), a stepped one is the value of the
  contributor with the most weight (the scene value counts with 1 - the states' weight; ties go to
  the later state, so a crossfade flips at its middle). Additive layers skip stepped tracks.
  Avatar masks and mixing transforms select transforms only: property tracks always apply.
- **A whole value and one of its components** (`color` and `color.a`) can be in one clip: the
  component track wins for its component (the Inspector warns).
- **GameObject active.** The flush applies active changes last, parents first, so the
  `onEnable` / `onDisable` they run never interrupt the player; clip events run after the values
  are written. A child the clip turns off keeps being animated (it can be turned on again). A clip
  that turns off its own root stops its player (an inactive GameObject's player waits), as in Unity.
- **A Button** writes its graphic's `ui.color` from its state colours every frame: animate
  `button.colorNormal` instead (the binding report says so).
- **Lua fields.** The clip sets the instance field `self.<name>` (a fresh Vector for vectors) of the
  first enabled script of that class on the GameObject, and the Inspector shows the live value.
  Read it in `onUpdate`; a value a script writes there is overwritten by the clip in the same
  frame's animation update, as in Unity (a write in `onPostUpdate` wins for that frame).
- **Missing components** (or a submesh, a shader member, a script field that is not there) leave
  the track unbound, reported as `missing_component`; a component added at runtime binds within a
  quarter of a second. Untouched values keep their scene value, and when the last state animating
  a property stops, the value stays as written.
- **The preview** shows property tracks like the others (an `active` key toggles the real flag) and
  puts every value back when it stops; nothing of it is saved.
- **Porting Unity clips.** `m_IsActive` steps are `gameobject.active` keys; Image `m_Color` and
  TMP `m_fontColor` are `ui.color` (keys in sRGB, as the colour pickers show them); a CanvasGroup's
  `m_Alpha` is `canvasgroup.alpha`; a sprite swap (`m_Sprite` PPtr curve) is `sprite.frame` keys
  of the frame indices. The UI's layout space is y-down where Unity's is y-up, so the y values
  convert: `m_AnchoredPosition.y` v is `recttransform.positionOffset.y` -v (Unity's keys 150, -70,
  0 are -150, 70, 0 here), `m_Pivot.y` v is `pivot.y` 1 - v,
  `m_AnchorMin.y` v is `anchorPointBottom` 1 - v and `m_AnchorMax.y` v is `anchorPointTop` 1 - v
  (min and max swap: Unity's bottom anchor is the larger y here), and `m_OffsetMin.y` /
  `m_OffsetMax.y` v are `anchorOffsetBottom` / `anchorOffsetTop` -v. x values port unchanged.

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

## Audio cues

A clip's Audio lane holds **audio cues**: at a time in the clip, play a sound (Unity's Timeline
audio clip, or an animation event that plays a sound, without the script). A cue is one of:

- **sound**: an audio clip or a Sound Event (`.soundevent`) played as its own voice, 2D or, with
  Spatial, following a GameObject of the animated hierarchy (a path from the root, `""` = the root),
  with its own volume (dB, -60 to 12), pitch (semitones, -24 to 24) and mixer group (default the
  event's, else Master). A Sound Event's own picks and ranges apply first;
- **source**: plays the AudioSource of a GameObject of the hierarchy (its clip or event, group and
  3D settings), so the clip's `audiosource` property tracks on the same GameObject shape it (an
  engine loop whose volume and pitch follow curves);
- **snapshot**: moves the project's mixer to a snapshot over Seconds (Unscaled: on unscaled time).

**Stop** is None (plays to its end) or State Exit (a 10 ms fade-out when the state that fired it
stops, is left or finishes its crossfade out; a source cue stops its source). **Follow Speed**
also scales the pitch by the state's speed.

Cues fire exactly as events do: forward, backward, across loop and ping-pong boundaries, once per
crossing, only for states that fire events; the Animator fires the leader clip's cues per layer
(Event Mode) and a montage its clip's. A cue starts on the sample its crossing falls on inside the
frame, so a footstep at 0.35 s plays at 0.35 s whatever the frame rate; the state's speed and the
time scale are already in that place. Pausing the scene pauses cue voices, a scene that unloads
stops them, `Audio.stopAll` stops sound cues. Edit-mode previews play nothing, except the Animation
window's playing preview, which plays the cues on preview voices outside the mixer (scrubbing is
silent; a snapshot cue does nothing there). A cue whose sound, path or snapshot does not resolve
does nothing and warns once per Play (`Audio: cue 2 of anim/run.anim: Body/LeftFoot binds nothing`).

The legacy action timeline (an entity Animation) plays the same sounds as **audio actions** (an
action GameObject with an AudioSource) and **Mixer Snapshot actions** (an `AudioSnapshotComponent`:
`snapshot`, `seconds`, `unscaled`, `revert` back to the previous snapshot on stop while the mixer
still shows its own, `revertSeconds`, -1 = `seconds`). They start once per loop pass, placed to
the sample by the same rule, and last their frame: an automatic duration is the sound's length, a
shorter one cuts it. Both paths start their voices through one function, so the same footsteps
placed either way give the same samples.

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
  `defaultClip`, `defaultFadeTime`, `playAutomatically`, `enabled`; `isLoaded()`;
  `getAnimatedProperties(name)` (the state's bound property tracks as `"path:component.property"`,
  for debugging).
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
- **Another character rig** (other bone names, rest orientations or proportions): renaming paths
  doesn't make a clip fit it. Rotation keys are absolute local rotations, so the other rig's bones
  end up turned wrong (legs up, arms twisted). Play it as a **humanoid clip** instead: tick the
  entry's **Retarget**, mark the source model **Animation Type: Humanoid**, or **Convert to
  Humanoid Clip...** (see [Humanoid clips and retargeting](#humanoid-clips-and-retargeting)). For
  agents, the binding report offers no rename fix for such paths (`rest_mismatch`) and suggests
  retargeting.

The files are referenced by GUID (the `guid` key the editor writes into each clip) plus the path.
Renaming or moving a clip keeps every Animation Player pointing at it: open scenes and loaded
prefabs right away, closed scenes and prefabs when they are opened (the scene opens as modified
with the repaired path). Deleting a clip leaves a yellow "Missing" entry, and undoing the delete
relinks it. A copied clip gets a new GUID.

## Humanoid clips and retargeting

A **humanoid clip** plays correctly on any humanoid character, whatever its bone names (Mixamo,
Unreal, Blender/Rigify, VRM, Unity), rest pose (T-pose or A-pose) and proportions: Unity's humanoid
(muscle) clips. Instead of bone paths it stores, per humanoid slot (`left_upper_arm`...), the joint
rotation relative to the **neutral pose** the pose system computes for every rig, plus the body
(hips) position and two foot goals in units of the rig's human scale (neutral hips height / 0.95).
The player turns those into bone rotations on its own character's rig every frame, so hips height
and stride scale with the character.

Three ways to get one, nothing is retargeted otherwise (old clips play exactly as before):

- **Animation Type: Humanoid** on the source model (Inspector > Model > Rig, stored in its
  `.humanoid` file, Unity's Rig tab). Every `model.glb#Clip` of it is then a humanoid clip, played
  retargeted by any player; Extract to .anim writes the humanoid clip.
- **Retarget** on a player entry: that generic clip plays retargeted from its source model (a
  model clip's model, or a `.anim`'s source or Preview Model), read through the source's rig as it
  plays, exactly as the Animator retargets (one retarget path: the same clip and options give the
  same pose on both; Animator states have Mirror, Foot IK and Retarget too).
- **Convert to Humanoid Clip...** (the `.anim`'s Project menu, the clip asset Inspector, a player
  entry's clip menu) writes an editable humanoid `.anim` (`<name> (Humanoid).anim`); from an entry,
  **Use in this player** switches it in the same undo step. The Pose window's **Bake > Bake to
  Humanoid Clip** bakes a pose animation as one.

The **target rig** is the first model on or under the player's GameObject whose bones map as a
humanoid. Without one the humanoid tracks don't play (the Bindings report says so) and the clip's
other tracks still do. A humanoid entry shows a **Humanoid** badge and two options:

- **Mirror**: left and right swapped (also Lua `state.mirror`).
- **Foot IK**: each leg reaches the clip's foot goal (scaled to this character), so feet don't
  slide when the legs are longer or shorter than the source's (Lua `state.footIK`). It runs per
  state before the layers blend.

The Bindings report lists, per humanoid state, the rig it plays on with its human scale and the
ratio to the source's, the slots a missing one was folded into ("chest folded into spine": a rig
without a chest bone gets the chest's rotation on its next bone), and the clip bones the rig lacks.
Fingers, eyes and the jaw play when both rigs map them; a slot the clip lacks stays in the neutral
pose, while a finger, eye or jaw bone the clip lacks is left to the other clips and layers (a
right-hand pose on an Override layer keeps the idle's left hand; with **Mirror** it poses the left
hand and leaves the right one). Props and bones that aren't humanoid stay normal tracks: they bind by path, or by a unique
bone name on another rig.

Humanoid clips blend with generic clips, layers, masks (a humanoid mask's body parts apply to their
bones) and additive layers. In the Animation window their rows read `Humanoid : Left Upper Arm`
with the slot's joint angles (the pose system's raise / forward / twist...) as curves, `Humanoid :
Body Position` and `Humanoid : Left Foot Goal`; **Add Property > Humanoid** adds a slot. Record
keys a rotated bone's joint rotation on a humanoid character (bind one to record a humanoid clip).
Fix a rig's mapping, fingers or twisted forearms in **Configure Avatar...** (see
[Pose animation](pose-animation.md#the-avatar-configuration-window)).

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

## Layers and masks

An Animation Player has **layers** (Unity's animation layers), listed in the Inspector's **Layers**
table above the clips. **Base Layer** is layer 0. Each clip entry plays on one layer (its **Layer**
combo), and each layer has:

- **Weight** (0 to 1): how much of the layer shows over the layers below it. The base layer is always
  1. `player:setLayerWeight("Holding", w, fade)` changes it at runtime (linear fades in the player's
  scaled time); the Unity pattern `w + (target - w) * dt * 15` works as is.
- **Blending**: **Override** replaces what the layers below produce, as much as the weight says.
  **Additive** adds the clip's change from its first frame (or from the clip's
  `additive_reference` time) to what is below: a nod, a breath, a hit reaction on top of any walk.
  The base layer is always Override.
- **Mask**: an **Avatar Mask** (`.mask`) that says which bones the layer animates. Without a mask, a
  layer animates exactly the channels its clips have keys for, and everything else passes through
  from below: an upper-body clip on layer 1 leaves the legs to the base layer.

`play`, `crossFade` and `queue` work per layer. `play("Hold")` on layer 1 does not stop the walk on
layer 0 (Unity's `PlayMode.StopSameLayer`); `play("Walk", 0, PlayMode.STOP_ALL)` stops every layer.
A queue waits only for its own layer. Events fire only from layers whose weight is above 0 (the base
layer always fires); a layer at weight 0 costs nothing but keeps its states' time going, so raising
the weight later shows the right phase. `onFinish` still fires on a zero-weight layer.

In play mode the **States** panel groups the states under their layers, with a live weight bar and
a slider that drives the runtime weight (not saved). The **Bindings** report adds, per masked
layer, how many bound bones its mask covers, and the mask's warnings.

The farmer of the Garden game, in Lua:

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

### Avatar Masks

**Assets > Create > Avatar Mask** makes `New Avatar Mask.mask`. Click it in the Project window to edit
it in the Inspector (project undo steps):

- **Default**: the weight of every bone no rule below covers. A new mask (and an empty one) passes
  everything, as in Unity.
- **Humanoid**: a figure with the body parts (Root, Body, Head, the arms, hands, fingers and legs).
  Click a part to toggle it; green is 1, red is 0, a tint is a fraction (hover for its weight; the
  slider under the figure sets a fraction for the part you clicked last). It
  works on any model with a humanoid rig (the same rig detection as Pose Animation: Mixamo, VRM and
  most naming schemes); "Not humanoid" shows when the preview model has none.
- **Transforms**: the bones of the **Preview Model** (or of the selected GameObject: **Use
  Selection**) as a tree. Tick a bone, set its weight, and choose whether it also covers its
  children. Shift-click ticks a whole subtree.
- **Users**: the players and layers (also of Animator Controllers) that use the mask.

Each tree row also shows, as a coloured dot, the weight the bone gets from the whole mask (hover it
for the rule: its transform entry, a body part, or Default).

Which rule wins for a bone: its own transform entry, else its nearest ancestor's entry that covers
children; else, when the mask has humanoid parts and the character is humanoid, the bone's body
part (an arm is the shoulder to the forearm, a hand its hand bone, fingers the bones below a hand,
twist bones take their parent bone's part; with **Root Hierarchy** Root is also the character and
the nodes above the hips; a part left unset weighs Default); else Default. The Animator's layers
read masks the same way (`docs/animator.md`). Mask weights are not normalized: a bone at
0.5 on an Override layer at weight 1 shows half the layer and half what is below.

The fastest way to a mask: select the bones in the Hierarchy, right-click the layer in the
Inspector and pick **Create Mask from Selection...** (it writes the mask and assigns it, as one undo
step). A mask names paths, not scene objects, so one mask works on every character with the same
rig. A layer whose mask is missing or still loading contributes nothing, and the Console says why
once; undoing the delete of the file relinks it.

In the Animation window, the clip menu groups states by layer (`Holding ▸ Hold`). For a state on a
layer above the base, **Preview over** picks a base state: the preview then shows that base state
at the playhead with the edited clip on its layer and mask, as in the game. Record still keys the
edited clip only. Rows the layer's mask hides are dimmed.

From Lua, a state can also be limited to some bones without a mask, like Unity's legacy
`AddMixingTransform`: `player:getState("Wave"):addMixingTransform("Hips/Spine/RightArm")` (the path
and, unless `recursive` is false, everything below it). Several mixing transforms add up; they
multiply with the layer's mask.

## Animator Controllers

An **Animator Controller** (`.controller`) is a state machine of clips and blend trees driven by
parameters: it plays on an **Animator** component, described in `docs/animator.md`. The Animation
Player's **Controller** row offers **Create Animator Controller from Clips**: one state per clip
entry (the default clip as the default state, the entries' layers as controller layers with their
weights, blends and masks), an Animator playing it added to the GameObject, and the player
disabled, all one undo step.

A disabled Animation Player beside an Animator **hands over** to it, so scripts written for a
player keep working on the Animator: `play`, `crossFade`, `stop`, `isPlaying`, `setFloat` /
`setInteger` / `setBool` / `setTrigger` / `resetTrigger`, `getCurrentStateName([layer])`,
`getCurrentStateInfo([layer])` / `getNextStateInfo([layer])` (a `ControllerStateInfo`: `isValid`,
`name`, `fullName`, `normalizedTime`, `length`, `speed`, `loop`), `isInTransition`,
`playInLayer(name, layer[, normalizedTime])`, `crossFadeInLayer(name, seconds, layer)`,
`setLayerWeight(name, w[, fade])`, and its `onStateEnter(layer, name)` / `onStateExit(layer, name)`
and `onEvent`. `GameObject:getAnimator()` returns the Animator (the player when there is no
Animator), which answers the same names:

```lua
function Farmer:init()
    self.animator = GameObject(self.scene, self.entity):getAnimator()
    RegisterEngineEvent(self, "onUpdate")
end
function Farmer:onUpdate()
    self.animator:setFloat("Speed", self.body.linearVelocity:length())
    self.animator:setLayerWeight("Holding", self.holding and 1 or 0, 0.1)
end
function Farmer:onStateEnter(layer, name)
    if name == "Watering" then self.can:setActive(true) end
end
```

`setFloat(name, v, dampTime)` eases the value with Unity's SmoothDamp. A call of the wrong type
warns once and converts. Scenes and prefabs saved when the player played controllers itself load
with an Animator playing that controller and the player disabled (the Console says how many).

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
| `format`, `version` | required (`"jasperengine.anim"`, `1`; `2` for a humanoid clip without cues; `3` for a clip with audio cues, the only clips written as version 3, so an older engine refuses them instead of dropping their sounds). A missing or different format, a newer version, text that isn't JSON (reported with `line:col`) or a Unity YAML clip fail to load |
| `guid` | the clip's identity for references. The editor adds one to a clip without it. Don't copy it into another file: a copy made in the Project window gets its own |
| `frame_rate` | samples per second, 1 to 240 (default 30): the snapping grid only. Key times are seconds |
| `wrap` | `once` (default), `loop`, `ping_pong`, `clamp_forever` |
| `length` | optional, in seconds. Without it, the last key, event or cue time. Longer holds the end pose, shorter cuts the tracks |
| `additive_reference` | optional, in seconds (default 0, written only when not 0): on an Additive layer the clip adds its change from the pose at this time. Clamped to the clip |
| `tracks[]` | `path`, `channel`, `keys`; optional `relative` (position, rotation, scale only) and `loop` (the track's keys repeat inside a longer clip) |
| `events[]` | `time`, `function` (a Lua identifier, not a keyword), `args` (at most 8 numbers, strings, booleans) |
| `audio[]` | written only when not empty, sorted by time: `time` and exactly one of `sound` (`{"guid", "path"}`, asset-relative), `source` (a path), `snapshot` (a name); sound: `path` + `spatial`, `volume_db`, `pitch_semitones`, `group`; snapshot: `seconds`, `unscaled`; any: `stop` (`none`, `state_exit`), `follow_speed`, `name`. A cue's time may pass the keys: it extends the length like an event. An invalid cue is dropped with a warning |
| `preview.model` | editor only: the model or prefab (asset-relative) to preview on when no GameObject is bound |
| `source` | written by Extract, Convert and Bake (`{"kind": "model" / "entity" / "pose", "path", "name"}`); ignored at runtime, except that a Retarget entry (or an Animator) retargets a `.anim` from its `source.path` model when `kind` is `model` (else from its `preview.model`) |
| `humanoid` | humanoid clips: `{"source_model", "human_scale"}` of the rig it was made on (informational: the reports' scale ratio) |

Channels and values:

| channel | value |
|---|---|
| `position`, `scale` | `[x, y, z]`, local |
| `rotation` | a quaternion `[x, y, z, w]`, or `{"euler": [x, y, z]}` in degrees (read, written back as a quaternion). Between two Euler keys more than 180 degrees apart on an axis, add a middle key (the loader warns). Keys are written on the side of the key before them (the same rotation); a track with tangents keeps its signs, since its curve depends on them |
| `morph` | `[w0, w1, ...]`, one weight per morph target. Shorter arrays mean 0 for the rest; at most 8 targets |
| `lattice` | `{"points": [[flatIndex, [x, y, z]], ...]}` offsets of the cage points. A point a key omits holds 0 (warning). The path is the cage GameObject |
| `property` | a component property: the track also names `component` and `property` (see [Property tracks](#property-tracks)); the value is `true` / `false`, an integer, a number, or an array of 2 to 4 numbers (colours `[r, g, b(, a)]` in sRGB, or `"#RRGGBB[AA]"`) |
| `humanoid` | no `path`: `"bone"` names a slot (`hips`, `left_upper_arm`... the pose system's 22) or a finger, eye or jaw (`left_index_proximal`, `left_eye`, `jaw`). A quaternion `[x, y, z, w]`: a slot's joint rotation relative to the neutral pose, a finger's rotation from its rest, in the character frame. Slots also take `{"angles": [dof1, dof2, twist]}` in degrees (written back as a quaternion) |
| `humanoid_position` | no `path`: `"bone"` is `hips` (the body offset) or `left_foot` / `right_foot` (the Foot IK goals); `[left, up, forward]` from the neutral hips, in human-scale units |

- A key is `[time, value]` or `[time, value, ease]`. `ease` is the easing of the segment that
  starts at that key: `linear` (default), `step`, or
  `<quad|cubic|quart|quint|sine|expo|circ|back|elastic|bounce>_<in|out|in_out>`. A `step` segment
  holds its first key's value; at exactly the next key's time the value is that key's own (Unity's
  rule, the same in entity Animations). The object form
  `{"time", "value", "ease", "in", "out", "tangent_mode"}` carries Hermite tangents (glTF
  CUBICSPLINE, value units per second). Tangents win over an ease (except `step`: a Constant
  segment), and a track in which only some keys have tangents gives the others linear ones
  (warning). `tangent_mode` (`auto`, `free`, `flat`, `broken`, `linear`) is the editor's hint of
  how the curves view made them; the engine plays the tangents.
- `relative`: position adds to the target's rest (scene) value; rotation and scale multiply it, so
  `[1, 1, 1]` is "unchanged" for a relative scale.
- Everything but `format`/`version` is lenient. A bad key, track or event is dropped with a warning
  that carries its JSON path (`tracks[3].keys[2].value: expected [x, y, z]`), and the rest loads.
  Keys out of order are sorted, keys closer than a microsecond collapse (the later wins), and
  duplicate (path, channel) tracks merge (property tracks: the later of two with the same
  component and property wins). Unknown top-level keys are kept when the editor rewrites the file.
  A property track of a component or property this engine cannot animate is kept as it is and not
  played (the warning names the nearest animatable property).
- The load warnings show in the clip asset Inspector, in `jasperengine-editor check --anims`, and to
  agents in `external_changes.problems`.

In a scene or prefab, an Animation Player is saved as:

```yaml
animationplayer:
  enabled: true
  clips:
    - {clip: anim/walk.anim, guid: 6f1d0c2a9b8e4f7da1c3e5b7092f4d6e, name: Walk, wrap: loop}
    - {clip: models/hero.gltf#Idle}
    - {clip: models/robot.glb#Run, retarget: true, footIK: true}
  defaultClip: Walk
  playAutomatically: true
  speed: 1
  defaultFadeTime: 0.25
```

`clip` paths are relative to the assets folder. `guid` is written for `.anim` files only (model
clips are path-only), and `name`/`wrap`/`retarget`/`mirror`/`footIK` only when set. With layers:

```yaml
animationplayer:
  clips:
    - {clip: models/garden/granjero.glb#Running, name: Running, wrap: loop}
    - {clip: anim/holding.anim, guid: ..., name: Holding, wrap: loop, layer: 1}
  layers:
    - {name: Base Layer}
    - {name: Holding, weight: 0, blend: override, mask: anim/arm.mask, maskGuid: ...}
```

`layer` is written only when not 0, `layers` only when they differ from one Base Layer without a
mask, a layer's `weight` only when not 1 and `blend` only when not override. Masks follow the clip
GUID rules: a rename repairs the path, a deleted file shows Missing. A `controller` key (files saved
when the player played controllers) is moved to an Animator by the scene migration (scene version
6, prefab version 7); found in a newer file it is not read (a warning).

### The .mask and .controller files

Both are the Animator's files (`docs/animator.md`, "Files"): canonical, lenient JSON like `.anim`
(bad entries are dropped with a warning naming their JSON path, unknown keys are kept), version 2.
An Avatar Mask:

```json
{
  "format": "jasperengine.mask",
  "version": 2,
  "guid": "3c1e...",
  "default": 0,
  "body_parts": {"right_arm": 1, "right_hand": 1, "right_fingers": 1},
  "root_hierarchy": false,
  "paths": [
    {"path": "Granjero/mixamorig_Hips/mixamorig_Spine/mixamorig_Spine1/mixamorig_Spine2/mixamorig_RightShoulder/Can", "weight": 1, "children": true}
  ],
  "preview": {"model": "models/garden/granjero.glb"}
}
```

Body parts are `root`, `body`, `head`, `left_arm`, `right_arm`, `left_hand`, `right_hand`,
`left_fingers`, `right_fingers`, `left_leg`, `right_leg` (0 to 1); parts not listed take
`default`. Version 1 masks load converted: the earlier `humanoid` / `transforms` form (an arm
there also weighs the hand; transforms cover their children unless `children: false`) and the
Animator's earlier true / false parts. A controller's layers nest a `state_machine` (states with
their transitions, Any State, Entry, sub-state machines): `docs/mcp/guide/animator.md` lists every
member. Version 1 controllers with flat `states` and `transitions` (`"from": "any"`) load
converted, members such as `mirror` and `foot_ik` kept. Unity's YAML `.mask` and `.controller`
files are refused with a message naming the menu that makes a JasperEngine one.

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

For the Animator's features (sub-state machines, 2D and direct blend trees, IK, root motion,
clip overrides) see `docs/animator.md`. Not in this version: synced layers, IK in masks, importing
Unity `.controller` / `.mask` files (they are refused with a message), reading Unity muscle `.anim`
clips (re-export them as glTF and retarget), root motion from humanoid clips, root motion and
animating layer weights from clips (use Lua). Property tracks do not animate strings, textures,
fonts or meshes (only sprite frames swap), C++ script fields, properties that rebuild GPU state when
they change (shader, blend mode, cast shadows, font size, shadow map size), physics body values
other than Enabled, or the Animation Player's own Enabled. Also: importing Unity curves, tangent
editing of rotations by handle (they take a tangent mode), importer clip ranges and per-model-clip
events, a preview pane in the clip asset Inspector, event object parameters, and re-syncing an
extracted clip from its model.
