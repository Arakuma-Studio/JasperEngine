# Camera shake and camera follow

Two components shake and move cameras: the **Camera Shaker** and the **Camera Follow**. Both
run every frame after your scripts' `onUpdate`, animations and physics, and before the frame is
drawn. The full reference, written for coding agents but useful to anyone, is
[docs/mcp/guide/camera.md](mcp/guide/camera.md). The Lua API is in
[lua-api.md](mcp/guide/lua-api.md), section 4.9.1.

## Camera Shaker

Add it to a camera: **Add Component > Rendering > Camera Shaker**, or **GameObject > Add Camera
Shaker to Main Camera**. Then shake from any script:

```lua
CameraShake.shake("explosion")
```

The shake moves what the camera **sees**, not the camera itself. Clicks and touches picked
through the camera, and its world direction / up / right vectors, follow the shaken view (as in
Unity); its position, rotation, target and the 3D audio listener do not. Its Transform keeps its
position and rotation. A camera that follows a player, or that a script moves every frame, still
shakes, and nothing has to be restored afterwards. If you come from MilkShake, you no longer need
an extra child object for the camera: put the Camera Shaker on the camera itself.

### Presets

A shake plays a **Shake Preset**. A preset has a type, a strength, a roughness, a fade in, a fade
out, and position and rotation influences per axis. MilkShake presets have the same values.

- **One shot** presets fade in and then out: explosions, punches.
- **Sustained** presets hold until stopped: drunk, off-road, electrocute.

```lua
self.heavy = CameraShake.shake("earthquake")   -- keep the handle...
self.heavy:stop(0.2)                           -- ...and fade it out over 0.2 s
```

The engine ships 16 built-in presets: `explosion`, `light_explosion`, `earthquake`, `punch`,
`shock`, `electrocute`, `drunk`, `offroad`, `jumpscare_2d`, `eh_shake`, `entity_shake`,
`invader_drunk`, `invaders_explosion`, `handheld`, `vibration` and `bump`.

To make your own, use **Assets > Create > Shake Preset** (or Create > Shake Preset in the Project
window). It writes `New Shake Preset.shake`.

- Select it to edit its values. The graph below the fields shows the camera motion it makes and
  redraws as you drag a value.
- **Load Built-in** copies a built-in's values into the file.
- **Test** shakes the scene's main camera in the Game view.
- Every edit can be undone.

Play the file from Lua by its path, `CameraShake.shake("assets/shakes/boss_stomp.shake")`, or
choose it as the shaker's **Start Preset**, which plays when the game starts and each time the
shaker is enabled.

### Trauma

For hits and damage, add **trauma** instead of starting a shake:

```lua
CameraShake.addTrauma(0.4)
```

Trauma is a value from 0 to 1 that decays over time. The shake grows with trauma squared (the
**Power** setting), so small hits stay subtle and big ones read clearly. The **Trauma** foldout in
the Inspector sets the decay, the power, the largest offset and angle, and the frequency.

### 2D

A HUD drawn by the same camera as the world shakes with it: put the HUD in a separate UI scene
(attached as a child scene, with its own UI camera) so only the world shakes.

On an orthographic or UI camera (**Mode** Auto, or 2D), only x/y movement and roll are applied.
Distances are measured in tenths of the view height, so a preset looks the same at any camera
size, as in Unity's 2D camera.

### Testing in the editor

The **Test** strip in the Camera Shaker's Inspector has a preset menu, **Shake**,
**+ Trauma 0.5** and **Stop**.

- **Edit mode.** Test previews in the Game view, which shows a "Shake preview" badge. The scene
  is not changed. Play, saving or selecting something else stops the preview.
- **Play mode.** Test shakes the running game. While the game is paused, shakes asked by scripts
  or tools are dropped (a warning says so).

A readout shows the trauma, the number of active shakes and the current offset.

### Time

Shakes use the game's time by default: `Time.timeScale = 0` freezes them. Turn on **Unscaled
Time** on the shaker, or pass `{unscaled = true}` to one shake, to keep them going in a pause
menu.

### Shaking something that is not a camera

On a GameObject without a Camera, the Camera Shaker moves the Transform itself. It adds the shake
on top of whatever else moves it, and puts the GameObject back when the shake ends. A UI Image
shakes this way for a jumpscare. GameObjects with a Rigidbody are not shaken: shake a child
instead.

## Camera Follow

Add **Camera Follow** to a camera and set its **Target**:

- **Offset** is where the camera stays relative to the target. **Offset Space** World keeps the
  offset fixed; Target turns it with the target, and Target Yaw turns it with the target's
  heading only, like a chase camera.
- **Damping** is the time in seconds the camera takes to catch up, per axis, with Unity's
  `SmoothDamp`. 0 keeps it rigid. **Max Speed** caps how fast it moves.
- **Look At** aims the camera at the target, plus **Offset** (aim at the head, not the feet).
- **Dead Zone** (orthographic and UI cameras) is a rectangle the target can move in without the
  camera following. It is the classic 2D platformer camera.
- **Bounds** keeps the camera inside a box, for example the level's edges.
- **Snap Now** places the camera at once. In edit mode this is one undo step.

The Scene view draws the bounds box and the dead zone of the selected Camera Follow.

A level's camera can follow a player that survives scene loads (`DontDestroyOnLoad`): set
`follow.target = player` from a script. A target in any other scene is refused.

While it is enabled, Camera Follow owns the camera's Transform. If a script also moves the
camera, Camera Follow wins and the Inspector warns you. A Camera Shaker on the same camera shakes
on top of the followed position.
