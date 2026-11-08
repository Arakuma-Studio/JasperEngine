# Camera shake and camera follow

Served as `jasper://docs/camera`. Two components, both run by the scene's `CameraControlSystem`:

- **Camera Shaker** (`camerashaker`): MilkShake's Shaker. It plays shakes from **Shake Presets**
  (built-in names or `.shake` files) and a **trauma** signal. On a camera the shake is a view
  offset: the camera's Transform is never written.
- **Camera Follow** (`camerafollow`): a small Cinemachine-like follower. It keeps a GameObject at an
  offset from a target with Unity's `SmoothDamp`, aims at the target, and has a 2D dead zone and
  world bounds.

User guide: `docs/camera.md`. Lua reference: `jasper://docs/lua-api`, section 4.9.1.

## Frame order

Each frame, `Scene::update` runs the systems in order. Animations and physics interpolation come
first, and Lua `onUpdate` has run before them. Then **Camera Follow**, then the **Camera
Shakers**, then the UI layout and `RenderSystem`, which builds the view matrices. So:

- A camera a script moves in `onUpdate` shakes around its new pose the same frame.
- A target moved in `onUpdate`, by physics or by an animation is followed the same frame. The
  follower composes the target's local transforms itself and does not use the world matrices
  cached by the last render.
- Writes made in `onPostUpdate` reach the camera one frame late.
- A held simulation or a paused app freezes everything. The offset holds.

## Shakes

```lua
CameraShake.shake("explosion")                                   -- every global shaker
local rumble = CameraShake.shake("earthquake", {strength = 0.5})
rumble:stop(0.2)                                                 -- fade out over 0.2 s
CameraShake.shakeAt(blast.position, 20, "explosion")             -- falloff max(0, 1 - d/20)
CameraShake.addTrauma(0.4)                                       -- Eiserloh trauma
self.gameObject:getCameraShaker():shake("assets/shakes/boom.shake", {seed = 7, unscaled = true})
```

- **Which shakers.** `CameraShake.*` calls reach every **global** shaker: `global` on, enabled,
  active in the hierarchy, in every running scene, child scenes included. Each shaker gets its
  own instance.
  - The returned `ShakeInstance` groups those instances. Setters change all of them. Getters read
    the first one still alive.
  - With no global shaker, the handle is invalid and one warning is logged.
  - `CameraShaker:shake` reaches one shaker, whatever its `global` flag.
- **Presets.** A preset is one of:
  - a built-in name (the table below);
  - a `.shake` path relative to the assets folder, the same path you give MCP. In the default
    layout (assets folder = project folder) that is `"assets/shakes/boom.shake"`;
  - a table `{type = "one_shot"|"sustained", strength, roughness, fadeIn, fadeOut,
    positionInfluence = Vector3, rotationInfluence = Vector3}`. Missing keys take the `.shake`
    defaults.

  An unknown name, or a `.shake` that does not exist or does not parse, logs one warning per name
  and shakes nothing. A wrong argument type is a Lua error.
- **Options** `{strength, roughness, seed, unscaled, fadeIn, fadeOut}`. `strength` and
  `roughness` scale the preset's values. The same `seed` gives the same motion. `unscaled`
  overrides the shaker's `unscaledTime` for this shake. `fadeIn` and `fadeOut` override the
  preset's.
- **One-shot or sustained.** A one-shot shake fades in, then fades out right away. A sustained
  shake holds until `stop([fade])`.
- **ShakeInstance members.**
  - `pause([fade])` and `resume([fade])` slow the noise to a stop and back. A paused sustained
    shake holds its offset. A one-shot shake keeps fading while paused, as in MilkShake.
  - `isPaused()`, `isFinished()`, `isValid()`.
  - `strengthScale` and `roughnessScale`.
  - `state`: one of `"fading_in"`, `"sustained"`, `"fading_out"`, `"paused"`, `"stopped"`.
  - `count`: how many shakers it is playing on.
- **Limits.** A shaker plays at most 32 shakes. When a 33rd starts, the oldest one-shot is
  dropped, with one warning.
- **Lifetime.** Shakes belong to their shaker. When the shaker's GameObject is destroyed, the
  component is removed or the scene unloads, the shakes are gone and their handles become
  invalid. `stop()` on an invalid handle is a no-op.
  - Disabling the shaker, or deactivating its GameObject, removes the offset. The shakes and the
    trauma hold until it is enabled again.
  - `startPreset` plays again on an enable, like Unity's `OnEnable`, unless the shake it started
    is still playing (it held while disabled): disabling and enabling never piles it up.

### MilkShake's formulas

The formulas match MilkShake, so the game's presets port with the same numbers.

- The noise is `N(x, y) = (perlin(x, y) - 0.5) * 2`. The engine's Perlin noise is in [0, 1], like
  Unity's `Mathf.PerlinNoise`, but the values are not bit-identical.
- The seeds are `b`, `b/2`, `b/3` and `b/4`. The clock starts at `b` and advances by
  `dt * roughness * fade`.
- The offset of a frame is the state **before** that frame's step, as in MilkShake's
  `UpdateShake`. The first frame of a shake is still.
- Rotations are Euler degrees applied Z, then X, then Y, as in Unity's `localEulerAngles`.
- The axes are the camera's local axes: x to the right, y up, z along the view.

### Units

**3D.** Position offsets are in world units and rotation offsets in degrees. `positionScale`
and `rotationScale` multiply them.

**2D.** The 2D rule applies with `mode` set to `"2d"`, or with `"auto"` (the default) on an
orthographic or UI camera:

- Only x/y movement and roll (z rotation) are applied.
- One preset unit is a tenth of the view height (`|topClip - bottomClip| / 10`), as in Unity's
  default 2D camera, which is 10 units tall. Presets look the same in both engines.
- The roll turns about the view's centre.

### Trauma

`addTrauma(x)` adds to a value in [0, 1]. The shake amplitude is `trauma ^ traumaPower`
(`traumaPower` is in [1, 4], default 2), and trauma decays by `traumaDecay` per second. At
trauma 1 the shake reaches `traumaMaxOffset` (units) and `traumaMaxAngle` (degrees).
`traumaFrequency` sets the noise speed.

Use trauma for hits and damage, which pile up and settle. Use presets for authored events: an
explosion, an earthquake, driving off-road.

### Built-in presets

`CameraShake.getPresetNames()` lists them. The first thirteen are the ported game's MilkShake
assets. `handheld`, `vibration` and `bump` come from EZ Camera Shake (strength = magnitude / 2).

| name | type | strength | roughness | fade_in | fade_out | position | rotation |
|---|---|---|---|---|---|---|---|
| `explosion` | one_shot | 0.2 | 15 | 0.05 | 1 | 1, 1, 0 | 20, 20, 20 |
| `light_explosion` | one_shot | 0.1 | 12 | 0.05 | 1 | 1, 1, 0 | 20, 20, 20 |
| `earthquake` | one_shot | 0.1 | 15 | 0.04 | 1 | 1, 1, 0 | 0, 0, 30 |
| `punch` | one_shot | 0.5 | 2.5 | 0.05 | 1 | 4, 0, 0 | 20, 90, 20 |
| `shock` | one_shot | 0.05 | 50 | 0 | 0.5 | 0, 0, 0 | 10, 60, 10 |
| `electrocute` | sustained | 0.05 | 20 | 0 | 0 | 0, 0, 0 | 0, 40, 40 |
| `drunk` | sustained | 0.1 | 0.35 | 0 | 0 | 0, 0, 0 | 5, 22.5, 15 |
| `offroad` | sustained | 0.25 | 3 | 1 | 1 | 0.1, 1, 0.1 | 20, 1, 1 |
| `jumpscare_2d` | sustained | 0.2 | 15 | 0 | 0 | 0, 0, 0 | 0, 0, 40 |
| `eh_shake` | sustained | 0.1 | 1.5 | 0 | 3 | 0.5, 1, 0.5 | 7.5, 2.5, 7.5 |
| `entity_shake` | one_shot | 0.2 | 15 | 7 | 2 | 0, 0, 0 | 20, 0, 20 |
| `invader_drunk` | sustained | 0.1 | 0.5 | 0 | 2 | 0, 0, 0 | 3, 3, 3 |
| `invaders_explosion` | one_shot | 0.3 | 10 | 0.01 | 0.6 | 1, 1, 0 | 0, 0, 3 |
| `handheld` | sustained | 0.5 | 0.25 | 1 | 1 | 0, 0, 0 | 1, 0.5, 0.5 |
| `vibration` | sustained | 0.2 | 20 | 0.5 | 0.5 | 0, 0.15, 0 | 1.25, 0, 4 |
| `bump` | one_shot | 1.25 | 4 | 0.1 | 0.75 | 0.15, 0.15, 0.15 | 1, 1, 1 |

## The `.shake` file

```json
{
  "format": "jasperengine.shake",
  "version": 1,
  "type": "one_shot",
  "strength": 0.2,
  "roughness": 15,
  "fade_in": 0.05,
  "fade_out": 1,
  "position_influence": [1, 1, 0],
  "rotation_influence": [20, 20, 20]
}
```

- **Defaults.** `type` one_shot, `strength` 1, `roughness` 1, fades 0 (instant), influences
  `[1, 1, 1]`.
- **Leniency.** A missing or wrong `format` or `version`, or text that is not JSON, is an error.
  Everything else is repaired with a warning: a negative value becomes 0, a bad type becomes
  one_shot, a bad vector takes its default.
- **Unknown keys** are kept and written back.
- **Writing.** `write_file` refuses a `.shake` it cannot read and returns the warnings of one it
  repaired. Assets > Create > Shake Preset writes `New Shake Preset.shake` with the `explosion`
  values.
- **Reloading.** An edited file plays its new values on the next shake. There is no reload step.

## Camera Follow

| property | default | meaning |
|---|---|---|
| `target` | none | the GameObject followed (same scene); none = the follower does nothing |
| `offset` | (0, 2, -6) | from the target, in `offsetSpace` |
| `offsetSpace` | `world` | `world`, `target` (turns with the target), `target_yaw` (turns with its heading) |
| `damping` | (0.2, 0.2, 0.2) | SmoothDamp smooth time per world axis, seconds; 0 = rigid |
| `maxSpeed` | 0 | SmoothDamp max speed; 0 = unlimited |
| `lookAt` | true | aim at `target + lookAtOffset`. A `useTarget` camera (the default) gets its `target` set; any other GameObject is turned (a camera looks down -Z) |
| `lookAtOffset` | (0, 1, 0) | world offset of the look point |
| `rotationDamping` | 0.1 | seconds to cover 99% of a turn (rotation look-at only); 0 = rigid |
| `deadZone` | (0, 0) | width, height in world x/y: target moves inside it do not move the camera (orthographic and UI cameras only) |
| `useBounds`, `boundsMin`, `boundsMax` | off | the follower's position is clamped to the box (world) |
| `unscaledTime` | false | follow with the unscaled frame time |

- **Snapping.** The follower is placed without damping on its first update, when the target
  changes, and after `snap()` (Lua) or **Snap Now** (Inspector).
- **Owning the Transform.** While enabled, the follower owns its Transform. If a script or an
  animation also moves it, the follower overwrites the move and warns once. The Inspector shows
  the warning, and so does `camerafollow.runtime.foreign_write`. To control the camera yourself
  for a moment, disable the Camera Follow.
- **Target gone.** When the target is destroyed, the follower holds where it is and warns once.
- **A persistent target.** A level's camera can follow a player kept with `DontDestroyOnLoad`:
  set `follow.target = player` from Lua (a GameObject of the persistent scene), or let the move
  happen: a follower of a GameObject that moves into the persistent scene follows it there. A
  target in any other scene is refused with a warning. `camerafollow.runtime.target_persistent`
  says so over MCP.
- **Shake on top.** A Camera Shaker on the same camera shakes around the followed pose.

## MCP recipes

```text
add_component {entity: "Main Camera", component: "camerashaker"}
set_properties {entity: "Main Camera", values: {camerashaker: {startPreset: "drunk"}}}
write_file {path: "assets/shakes/boom.shake", content: "{\"format\": \"jasperengine.shake\", \"version\": 1, ...}"}
get_asset_info {path: "assets/shakes/boom.shake"}     -> shake {values}, peak {position, rotation, duration}
list_assets {type: "shake"}
play {action: "start"}
eval_lua {code: "CameraShake.shake('assets/shakes/boom.shake')"}
get_runtime_entity {entity: "Main Camera", components: ["camerashaker"]}
  -> camerashaker.runtime {instances [{preset, state, strength}], trauma, offset_position, offset_rotation}
screenshot {view: "game"}
```

**Checking a follower.**

1. Add it: `add_component {entity: "Main Camera", component: "camerafollow"}`.
2. Set the target: `set_properties {entity: "Main Camera", values: {camerafollow: {target: <Player's id>, damping: [0, 0, 0]}}}`.
3. Play, move the player (`set_runtime_property` or Lua), and read the camera's `position` with
   `get_runtime_entity`. Its
   `camerafollow.runtime` gives `velocity`, `desired` and `placed`.

**Validation.**

- `startPreset` is `""`, a built-in name, or an existing `.shake` that reads.
- `traumaPower` is in [1, 4].
- The scales, decay, frequency, speeds, dampings and the dead zone are >= 0.
- `mode` is `auto`, `3d` or `2d`. `offsetSpace` is `world`, `target` or `target_yaw`.

**Not shaken: the Transform.** A camera's `position` and `rotation`, over MCP and in Lua, are
never shaken. `offsetPosition` and `offsetRotation` on the CameraShaker give the offset of this
frame.

**What is shaken.** The drawn view is: picking (`screenToRay`), `getViewMatrix()` and
`getWorldDirection()` / `getWorldUp()` / `getWorldRight()` follow the shaken view, as they would
in Unity where the shaker moves the camera. The camera's `direction`, `right`, `target` and
Transform stay steady, and so does the 3D audio listener (it hears from the unshaken pose).

**2D HUDs.** A HUD drawn by the same camera as the 2D world shakes with it. Put the HUD in a UI
scene attached as a child scene (`set_child_scene`; its own UI camera): only the world shakes.

**Paused games.** While the editor's Play is paused (or in edit mode), `CameraShake.*` reaches no
shaker: the shake is dropped, not queued, with one warning ("the game is paused"). The
Inspector's Test previews in edit mode instead.

## Transform mode (no camera)

A Camera Shaker on a GameObject **without** a Camera moves its Transform additively. The game
uses this for a jumpscare Image.

- **Base rule.** At the start of each frame, before the scripts and animations, the shaker
  writes the base back; after them it applies base + offset. So `self.position = self.position +
  v * dt` moves the unshaken pose, and the shake never builds up. A value someone wrote between
  frames (an editor edit) becomes the new base.
- **Ending.** When the shake ends, the base is written back once.
- **Anchored UI elements** are shaken through their RectTransform's `positionOffset`, so the
  layout keeps the move.
- **UI elements** use the 2D rule with units of the canvas height / 10; other GameObjects in 2D
  mode use tenths of the scene camera's orthographic height.
- **Physics bodies.** A GameObject with a Rigidbody or Rigidbody2D is refused, with one warning.
  Shake a camera, or a child without a body.
- **Edit mode** shows no preview of transform mode. Test it in Play mode.

## Editor

- **Camera Shaker Inspector.**
  - It has the settings, Start Preset (a built-in, a project `.shake`, or one dropped from the
    Project window) and a Trauma foldout.
  - The **Test** strip has a preset, Shake, `+ Trauma 0.5` and Stop.
  - In edit mode, Test previews in the Game view, which shows a "Shake preview" badge. Only the
    camera's view moves. Nothing is saved, and Play, a save or another selection stops the
    preview.
  - While it plays, a readout shows the trauma, the number of shakes and the offset.
- **`.shake` asset Inspector.** It shows the fields with their units, Load Built-in, and Test on
  the scene's main camera. A graph shows the offset sampled at 60 Hz with seed 0 (Position or
  Rotation) and redraws on every edit. Edits are project undo steps.
- **Camera Follow Inspector.** It shows the settings and Snap Now (one undo step in edit mode).
  The Scene view draws the bounds box and the dead-zone rectangle.
- **Menus.** GameObject > Add Camera Shaker to Main Camera. Add Component > Rendering > Camera
  Shaker / Camera Follow.

## Costs

- A shaker with no shakes, no trauma and no offset costs nothing.
- An active shaker evaluates 6 Perlin noises per shake and 6 for trauma, plus one matrix product
  when the view is built.
- The budget for 4 shakers with 8 shakes each is 0.02 ms per frame on a desktop Release build,
  and the engine test asserts it.
- A follower costs one walk up its target's parents plus a SmoothDamp.

## Unity mapping

| Unity / asset | JasperEngine |
|---|---|
| MilkShake `Shaker` | Camera Shaker component (`CameraShaker` in Lua) |
| `Shaker.ShakeAll(preset)` / `GlobalShakers` | `CameraShake.shake(preset)` / `global` |
| `Shaker.Shake(preset, seed)` | `shaker:shake(preset, {seed = s})` |
| `ShakeFromPoint(point, maxDistance, ...)` | `shakeAt(point, radius, preset)` |
| `ShakePreset` asset | `.shake` file or a built-in name |
| `ShakeInstance.Stop / Pause / Resume / StrengthScale / RoughnessScale` | `ShakeInstance:stop / pause / resume`, `strengthScale`, `roughnessScale` |
| Cinemachine Impulse Source / Listener | `CameraShake.shake` / `shakeAt`, Camera Shaker (gains: `positionScale`, `rotationScale`) |
| Cinemachine follow (Transposer + Composer, simple cases) | Camera Follow |
| `Mathf.SmoothDamp` (a hand-written follow) | Camera Follow `damping` (the same formula) |
