# Camera shake and camera follow (`camera-shake`, ledger #24)

Status: specced on 2026-10-31. Branch: `gaps/camera-shake`. This feature does not depend on the
animation features. It needs the unscaled frame delta, which the `coroutines` feature (#16) provides
(see section 17).

Read first: [AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md). Its shared conventions are binding,
in particular the rule on scaled and unscaled time and the rule on scene loads.

## 1. Goals

- A **Camera Shaker** component, equivalent to MilkShake's `Shaker`. You put it on any camera, 2D or
  3D. It plays **shake instances** created from **Shake Preset** assets (`.shake`) or from built-in
  presets. A shake is either one-shot or sustained. Its parameters are strength, roughness, fade in,
  fade out, position influence and rotation influence, and it uses Perlin noise. The noise formulas
  match MilkShake, so the game's 13 presets port with the same numbers.
- A **trauma** input on the same component, from Squirrel Eiserloh's "Juicing Your Cameras With
  Math": `addTrauma(x)`, shake = trauma^power, linear decay.
- **The shake is a view offset.** It is applied when the camera's view matrix is built. It never
  writes the camera's Transform. Cameras that follow targets, or that scripts move every frame, are
  never fought: no code needs to restore anything, and Lua reads of the transform are never shaken.
- Scaled time by default. Unscaled time per shaker, or per shake.
- Lua API:
  - static functions on `CameraShake`: `shake`, `shakeAt`, `addTrauma`, `stopAll`. They act on every
    global shaker, like MilkShake's `ShakeAll`;
  - per shaker: `getCameraShaker()`;
  - `ShakeInstance` handles: `stop`, `pause`, `resume`, and strength and roughness scales.
- Editor: an Inspector section with **Test** buttons and a live preview in the Game view, a
  Shake Preset asset Inspector with an offset graph, Assets > Create > Shake Preset, and MCP.
- **Shaking a non-camera GameObject**, used by the game's UI jumpscare `Image`. It is an additive
  Transform mode that does not fight script writes.
- **Phase 2 of this branch: a small Camera Follow component.** It covers the Cinemachine-lite cases
  that 10 of the game's 12 scenes hand-code: a follow offset, damping, look-at, a 2D dead zone and
  bounds. It runs right before the shake, in the same system.

## 2. Non-goals

- Cinemachine's virtual camera blending, priorities, composer framing, collision or occlusion
  avoidance, and spline dollies.
- Spatial impulse propagation with a travel speed. `shakeAt` gives a distance falloff only.
- Audio-driven shakes and per-channel listener masks. Shakes go to the `global` shakers, or to one
  shaker.
- Shaking physics bodies in Transform mode. Such a target is refused with a warning: use a camera,
  or a child without a body.
- Device vibration (haptics).

## 3. Users and stories

- **Porter.** Replaces `CameraShaker.instance.ExplosionShake()` with
  `CameraShake.shake("explosion")`. The camera keeps following the kart. `HeavyShake()` and
  `DisableHeavyShake()` map to keeping the handle and calling `inst:stop(0.2)`. The Menu's persistent
  "Drunk" shake is the shaker's **Start Preset**.
- **2D game developer.** Puts a Camera Shaker on an orthographic camera and calls
  `CameraShake.addTrauma(0.4)` on a hit. Only x/y movement and roll are applied, scaled to the view
  height.
- **Designer.** Duplicates the built-in `earthquake` preset into `shakes/boss_stomp.shake`, tunes
  strength and roughness while pressing **Test** in the Inspector during play, and watches the offset
  graph update.
- **AI agent.**
  - Adds the component with `add_component {entity: "Main Camera", component: "camerashaker"}` and
    writes a `.shake` file.
  - Tests with `play` and then `eval_lua "CameraShake.shake('explosion')"`.
  - Reads `get_runtime_entity` → `camerashaker.runtime: {instances, trauma, offset_position, offset_rotation}`
    to verify it worked.

## 4. Research summary

- **MilkShake** (Asset Store: [MilkShake Camera Shaker](https://assetstore.unity.com/packages/tools/camera/milkshake-camera-shaker-165604);
  the formulas below come from its source shipped in the game,
  `Assets/MilkShake/Scripts/Shaker.cs` and `ShakeInstance.cs`). Its parts:
  - presets with type, strength, roughness, fade in, fade out, position influence and rotation
    influence;
  - centred Perlin noise per axis, with per-instance seeds;
  - a noise clock that advances by `dt * roughness * fade`;
  - `ShakeFromPoint`, global shakers, and instance handles with stop, pause and resume.

  **Take:** all of the above, with the exact formulas, so the game's presets port unchanged.
  **Leave:** MilkShake *overwrites* `transform.localPosition` and `localEulerAngles` every frame.
  That forces the shaker onto a dedicated camera object with all movement on parents, which is how
  every scene of the game is set up. We apply the offset to the view instead, so any camera works.
- **EZ Camera Shake** ([GitHub](https://github.com/andersonaddo/EZ-Camera-Shake-Unity),
  `CameraShakePresets.cs`). It is the same model as MilkShake, by the same studio. **Take:** the
  preset names that users know (Bump, Explosion, Earthquake, HandheldCamera, Vibration,
  RoughDriving). The numbers are converted to MilkShake's scale (section 6).
- **Cinemachine Impulse** ([Impulse Source 2.8](https://docs.unity3d.com/Packages/com.unity.cinemachine@2.8/manual/CinemachineImpulseSource.html),
  [Impulse Listener](https://docs.unity3d.com/Packages/com.unity.cinemachine@3.1/manual/CinemachineImpulseListener.html)).
  **Take:**
  - the listener applies its signal *additively after the camera pipeline*, which is what our view
    offset does;
  - a gain per listener (our position and rotation scales);
  - a 2D mode;
  - a distance falloff (our `shakeAt`).

  **Leave:** channels, propagation speed and the attack/sustain/decay curve assets. Fade in and fade
  out cover the common uses.
- **Eiserloh, GDC 2016** ([GDC Vault](https://gdcvault.com/play/1023557/Math-for-Game-Programmers-Juicing),
  [Godot recipe](https://kidscancode.org/godot_recipes/4.x/2d/screen_shake/index.html)). **Take:**
  - trauma in [0, 1] with a linear decay;
  - shake = trauma^2 (configurable);
  - smooth Perlin noise per axis with its own seed;
  - in 3D, rotation preferred over translation (the trauma default gives a larger angle than offset);
  - in 2D, translation plus roll.
- **Follow damping.** Unity `Mathf.SmoothDamp` ([docs](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Mathf.SmoothDamp.html)),
  a critically damped spring, and Cinemachine's damper. **Take:** SmoothDamp per axis, with
  smoothTime = damping, because it is what the game's follow scripts use. **Take:** an exponential
  slerp for rotation damping.

## 5. Product decisions

1. **The shake is a render-only view offset.**
   - `CameraComponent` gets three runtime-only fields:
     - `bool hasViewOffset = false`;
     - `Vector3 viewOffsetPosition`;
     - `Quaternion viewOffsetRotation`.
   - `RenderSystem::updateCamera` (RenderSystem.cpp:11129) applies them after building the view
     matrix: `view = R⁻¹ · T(-p) · view`. This is a local offset in camera space.
   - Frustum planes, view-projection, `screenToRay`, mirrors and post effects all see the shaken view,
     as they do in Unity, where the shaker moves the transform.
   - The camera's Transform, `direction`, `right` and the transform's `worldPosition` are never
     changed. Gameplay code and the audio listener use the steady camera.
2. **One system.** A new `CameraControlSystem` (`engine/core/subsystem/CameraControlSystem.{h,cpp}`)
   is registered in `Scene::init` between `VertexAnimatorSystem` and `UISystem` (Scene.cpp:44-45).
   - It runs in `Scene::update`, after animations, physics interpolation and Lua `onUpdate`, and
     before `RenderSystem::update` computes matrices.
   - Each frame it runs Camera Follow (phase 2), then the shakers.
   - Lua writes made in `onPostUpdate` land one frame late for the camera. This is today's rule,
     because RenderSystem has already run by then. The docs say so.
3. **Time.**
   - A shaker's `unscaledTime` (default false) picks the time it uses: `dt` (scaled, the scene
     update's), or `Engine::getUnscaledDeltatime()` (#16). The `unscaled` option of a shake overrides
     it for that instance.
   - A held simulation or a paused app freezes shakes, because the scene update does not run then.
     The offset holds.
   - With `timeScale = 0`, scaled shakes freeze in place and unscaled ones keep going.
4. **2D cameras.**
   - Mode `auto` (the default) treats `CAMERA_ORTHO` and `CAMERA_UI` as 2D. Modes `3d` and `2d` force
     a choice.
   - In 2D only position x/y and rotation z (roll) apply.
   - Position units are scaled by `(topClip - bottomClip) / 10`. One preset unit is a tenth of the
     view height, as in Unity's default 2D camera (orthographic size 5, so 10 units tall). Presets
     look the same in both engines.
   - In 3D, position units are world units.
   - Both are then multiplied by `positionScale`. Rotations are multiplied by `rotationScale`.
   - **Rotation order.** Offset Euler angles are in degrees and are composed Z, then X, then Y,
     which is Unity's `localEulerAngles` order and MilkShake's.
5. **Global shakers.** `global` defaults to true. The static `CameraShake.*` functions act on every
   enabled shaker, active in hierarchy, with `global` set, in every scene in `Engine`'s scene list.
   That includes child scenes, and each scene's own camera.
   - The returned `ShakeInstance` handle groups the per-shaker instances.
   - MilkShake's `addToGlobalShakers` defaulted to off, and the game always called one shaker. Our
     default is on, because most games have one camera and `CameraShake.shake(...)` should just work.
6. **Lifetime** (brief rule). Instances belong to their shaker component and die with it: on destroy,
   on component removal, or on scene unload.
   - A handle whose instances are all gone is invalid. Its calls do nothing, and `isValid()` returns
     false.
   - Disabling the shaker, or deactivating its GameObject, pauses its instances and zeroes the offset.
     Enabling it again resumes them. Trauma is kept.
   - `startPreset` plays again on each enable (Unity `OnEnable`).
7. **Preset sources.**
   - A string is either a built-in name (no `.shake` extension) or an asset-relative `.shake` path.
   - A table defines the preset inline.
   - Unknown names log one warning each and do nothing.
   - `.shake` files load synchronously through `Data::open`. They are tiny. They are cached in
     `ShakePresetPool`, and editor edits invalidate the cache.
8. **Transform mode (non-camera GameObjects).** A Camera Shaker on a GameObject without a
   `CameraComponent` shakes its Transform additively. It uses the PoseBlendBone rule
   (PoseAnimatorRuntime.h:46-60):
   - When the local value still equals what the shaker wrote last frame, the base is the remembered
     base. Otherwise someone wrote it, and the new value is the base.
   - It writes `base + offset` (position) and `base * offsetRotation` (rotation).
   - When the shake ends, the base is written back once.
   - Anchored UI elements also get the `positionOffset` correction that `flushPoseBlend` uses
     (ActionSystem.cpp:1650-1658), so the layout does not snap them back.
   - Entities with a Rigidbody or Rigidbody2D are refused, with a warning once.
   - 2D rules apply when the entity has a `UIComponent` or `RectTransformComponent`: x/y and roll, in
     units of the canvas height / 10.
9. **Camera Follow is phase 2, on the same branch.** It is deliberately small: the five behaviours the
   game hand-codes (`CameraPosition`, `BasicFollow`, `GPlayerCamera`'s SmoothDamp, `CarCameraController`,
   `MoveXAlwaysForward`).
   - It **owns** the camera's Transform while it is enabled. A script that also moves that camera is
     overwritten, and a warning says so once when the system detects a foreign write.
   - The shake stays independent, as the view offset.

## 6. File format: Shake Preset `.shake`

```json
{
  "format": "jasperengine.shake",
  "version": 1,
  "guid": "…",
  "type": "one_shot",
  "strength": 0.2,
  "roughness": 15,
  "fade_in": 0.05,
  "fade_out": 1,
  "position_influence": [1, 1, 0],
  "rotation_influence": [20, 20, 20]
}
```

| key | default | meaning |
|---|---|---|
| `type` | `one_shot` | `one_shot`: fades in, then immediately fades out. `sustained`: fades in, then holds until `stop` |
| `strength` | 1 | amplitude multiplier (≥ 0) |
| `roughness` | 1 | noise speed (≥ 0). Higher means jerkier |
| `fade_in`, `fade_out` | 0, 0 | seconds. 0 means instant |
| `position_influence` | `[1, 1, 1]` | per-axis amplitude in units (section 5, decision 4) |
| `rotation_influence` | `[1, 1, 1]` | per-axis amplitude in degrees |

The format is lenient and canonical, with unknown keys kept (the same rules as `.anim`).
`engine/core/camera/ShakePreset.{h,cpp}` holds the parser and writer.

**Built-in presets** (`engine/core/camera/ShakePresets.cpp`; listed by `CameraShake.getPresetNames()`).
The first nine are the game's MilkShake assets (`Assets/MilkShake/Demo/Presets`). The last three are
EZ Camera Shake's, converted to MilkShake's scale: EZ amplitude ±0.5·magnitude, so
strength = magnitude/2.

| name | type | strength | roughness | fade_in | fade_out | position_influence | rotation_influence |
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
| `handheld` | sustained | 0.5 | 0.25 | 1 | 1 | 0, 0, 0 | 1, 0.5, 0.5 |
| `vibration` | sustained | 0.2 | 20 | 0.5 | 0.5 | 0, 0.15, 0 | 1.25, 0, 4 |
| `bump` | one_shot | 1.25 | 4 | 0.1 | 0.75 | 0.15, 0.15, 0.15 | 1, 1, 1 |

**Assets > Create > Shake Preset** writes `New Shake Preset.shake` with the `explosion` values. The
asset Inspector has **Load Built-in ▸ <name>** to copy any built-in's values.

## 7. Runtime C++

### 7.1 Components

```cpp
// engine/core/component/CameraShakerComponent.h
enum class CameraShakeMode : uint8_t { Auto, Mode3D, Mode2D };
struct JASPERENGINE_API CameraShakerComponent{
    bool enabled = true;
    bool global = true;                 // receives CameraShake.* static calls
    bool unscaledTime = false;
    CameraShakeMode mode = CameraShakeMode::Auto;
    float positionScale = 1.0f;         // multiplies every position offset (gain)
    float rotationScale = 1.0f;         // multiplies every rotation offset (gain)
    std::string startPreset;            // built-in name or .shake path; played on start and each enable ("" = none)
    // trauma (Eiserloh)
    float traumaDecay = 1.0f;           // trauma per second, linear
    float traumaPower = 2.0f;           // shake = trauma^power, [1, 4]
    Vector3 traumaMaxOffset = Vector3(0.1f, 0.1f, 0.0f);   // units at trauma 1 (2D: 1/10 view height units)
    Vector3 traumaMaxAngle = Vector3(3.0f, 3.0f, 5.0f);    // degrees at trauma 1
    float traumaFrequency = 15.0f;      // noise speed (MilkShake roughness units)
    // runtime (not serialised)
    std::shared_ptr<camera::ShakerRuntime> runtime;   // instances, trauma, seeds, applied offset, transform-mode base
};

// engine/core/component/CameraFollowComponent.h (phase 2)
enum class FollowOffsetSpace : uint8_t { World, Target, TargetYaw };
struct JASPERENGINE_API CameraFollowComponent{
    bool enabled = true;
    EntityReference target;             // required; nothing happens without one
    Vector3 offset = Vector3(0, 2, -6); // from the target, in offsetSpace
    FollowOffsetSpace offsetSpace = FollowOffsetSpace::World;
    Vector3 damping = Vector3(0.2f, 0.2f, 0.2f);  // SmoothDamp smoothTime per world axis, seconds; 0 = rigid
    float maxSpeed = 0.0f;              // 0 = unlimited (SmoothDamp maxSpeed)
    bool lookAt = true;                 // aim at the target (+ lookAtOffset)
    Vector3 lookAtOffset = Vector3(0, 1, 0);
    float rotationDamping = 0.1f;       // seconds to cover 99% of the turn; 0 = rigid
    Vector2 deadZone = Vector2(0, 0);   // width/height in the camera plane (units) where target moves don't move the camera
    bool useBounds = false;
    Vector3 boundsMin, boundsMax;       // the camera position is clamped to them (world)
    bool unscaledTime = false;
    // runtime: velocity (SmoothDamp state), last written local position/rotation, snapped flag
};
```

Both components follow the full new-component checklist of the brief:
- `ComponentType` entries appended at the end of the enum (Catalog.h:146-152, `LastComponentType`
  moved);
- the Catalog property tables and `hasEnabledProperty`;
- the Stream encode/decode;
- the Factory emitter (Factory.cpp next to :1916 and the dispatch at :2991);
- `EntityCloner` (the runtime is reset on clone);
- the Add Component popup entries next to Camera;
- the C++ script component events list (`editor/util/ScriptEvents.cpp:36` style), if events are
  added.

`CameraFollowComponent::target` is an entity reference and follows the existing reference
serialization (Entity/EntityReference properties).

### 7.2 Shake instance maths (`engine/core/camera/ShakeInstance.{h,cpp}`, pure)

- **Noise.** `N(x, y) = (perlin(x, y) - 0.5) * 2`. `perlin` is a new
  `engine/core/math/Noise.h` `float perlinNoise(float x, float y)`: improved Perlin, fixed
  permutation, output in [0, 1], 0.5 at lattice points. This matches the range of Unity's
  `Mathf.PerlinNoise`; the values need not be bit-identical.
- **Seeds.** For each instance:
  - `b` is the given seed, or a uniform random integer in [-10000, 10000].
  - `s1 = b / 2`, `s2 = b / 3`, `s3 = b / 4`.
  - `t` starts at `b`.
- **Each step `dt`** (with `paused` going 0 → 1 over the pause fade):
  - `fade` moves toward its target (1 while fading in or sustained, 0 while fading out) at
    `dt / fade_in` or `dt / fade_out`. A zero time is instant. `fade` is clamped to [0, 1].
  - When `fade` reaches 1: a sustained shake becomes `Sustained`, and a one-shot starts fading out
    with `fade_out`.
  - When `fade` reaches 0 while fading out, the instance is `Stopped` and is removed.
  - `strength = preset.strength * fade * strengthScale`, and
    `roughness = preset.roughness * fade * roughnessScale`.
  - `t += (1 - paused) * dt * roughness`.
- **Offsets.**
  - Position: `(N(t + s1, b), N(b, t), N(s3 + t, b + t)) * strength ⊙ position_influence`.
  - Rotation in degrees: `(N(t - b, s3), N(b, t + s2), N(b + t, s1 + t)) * strength ⊙ rotation_influence`.
- **`shakeAt(point, radius)`.** It scales `strengthScale` and `roughnessScale` per shaker by
  `max(0, 1 - d/radius)`. `d` is the distance from the camera's world position (3D), or the xy
  distance (2D). A scale of 0 creates no instance.
- **Trauma.**
  - Each step: `trauma = max(0, trauma - traumaDecay * dt)`.
  - `k = trauma ^ traumaPower`, and the trauma clock is `τ += dt * traumaFrequency`.
  - Position: `traumaMaxOffset ⊙ (N(τ + 11, sT), N(sT, τ + 23), N(τ + 37, sT + 5)) * k`.
  - Rotation: `traumaMaxAngle ⊙ (N(τ + 41, sT + 3), N(sT + 7, τ + 53), N(τ + 61, sT + 9)) * k`.
  - `sT` is a per-shaker seed.
  - `addTrauma(a)` sets `trauma = clamp(trauma + a, 0, 1)`.
- **Total.** The offset is the sum of all instances plus trauma. In 2D, only (x, y) and roll z are
  kept. Units and gains are applied as described in section 5, decision 4. Then
  `viewOffsetRotation = Euler(Z, then X, then Y)`.
- **Caps.** At most 32 live instances per shaker. Beyond that, the oldest one-shot is removed, with
  a warning once.

### 7.3 System (`CameraControlSystem::update(dt)`)

1. **Camera Follow** (phase 2). For each enabled follower that is active in hierarchy and has a
   resolved target:
   - It computes the target's **current** world transform by composing local TRS up the parent chain
     (`computeWorldNow`). The cached `Transform::worldPosition` is from the last render update, so
     moves made this frame by physics, animation and `onUpdate` would be missed.
   - Desired position: `targetPos + offset`, where the offset is in the world, target or target-yaw
     space.
   - With a dead zone: in the camera plane, the desired position only moves when the target leaves
     the rectangle `deadZone` centred on the current position minus the offset.
   - The desired position is clamped to the bounds.
   - Per axis, `SmoothDamp(current, desired, velocity, damping, maxSpeed, dt)`. This is Unity's
     formula: `ω = 2/smoothTime`, `x = ω·dt`, `e = 1/(1 + x + 0.48x² + 0.235x³)`, with the
     overshoot guard. Zero damping snaps.
   - Look-at:
     - With `camera.useTarget`, it writes `camera.target`, the look point in the camera's parent
       space.
     - Otherwise, it writes the rotation `slerp(current, lookRotation(dir, up), 1 - exp(-4.6 * dt / rotationDamping))`.
   - It writes the local position and rotation through the parent's inverse world matrix, and sets
     `needUpdate`.
   - On its first update, when the target changes, or after `snap()`, it places the camera without
     damping.
2. **Shakers.** For each shaker:
   - It steps the instances and the trauma with the scaled or unscaled dt, and plays `startPreset`
     on enable edges.
   - **Camera mode.** It sets `camera.hasViewOffset`, `viewOffsetPosition` and `viewOffsetRotation`,
     plus `camera.needUpdate = true` while the offset is non-zero or just became zero. With nothing
     to apply, `hasViewOffset` is false.
   - **Transform mode.** It applies decision 8.
   - A disabled or inactive shaker clears its offset once: `hasViewOffset = false`, or the base is
     written back.
3. **Editor preview.** `previewShake(entity, preset, editorDt)` is driven by the Inspector in edit
   mode. It uses the same maths on a preview runtime that is not saved. Stopping, a scene save, Play
   or a scene switch clears the view offset. Edit mode never touches the Transform: transform mode
   shows no preview, and a note says so.

## 8. Lua API (`engine/core/script/binding/*`; stubs regenerated)

**Static `CameraShake`:**

| function | semantics |
|---|---|
| `CameraShake.shake(preset [, options]) -> ShakeInstance` | one instance on every global shaker (decision 5). Returns an invalid handle when there is none, with a warning once |
| `CameraShake.shakeAt(position, radius, preset [, options]) -> ShakeInstance` | the distance falloff from 7.2. `position` is a Vector3 |
| `CameraShake.addTrauma(amount)` | every global shaker |
| `CameraShake.stopAll([fade])` | every global shaker's instances. `fade` overrides `fade_out`; 0 means immediate |
| `CameraShake.getPresetNames() -> table` | the built-in names |

**`preset`** is a built-in name, a `.shake` path, or a table:
`{type = "one_shot"|"sustained", strength, roughness, fadeIn, fadeOut, positionInfluence = Vector3, rotationInfluence = Vector3}`.
Missing keys take the `.shake` defaults.

**`options`** (table, all optional):
- `strength` and `roughness` (scales, default 1, the same as `strengthScale`/`roughnessScale`);
- `seed` (integer);
- `unscaled` (bool);
- `fadeIn` and `fadeOut` (override the preset's).

**`CameraShaker`** (EntityHandle; `CameraShaker(scene, entity)` adds the component;
`GameObject:getCameraShaker()` returns nil without one, `hasCameraShaker()`):
- methods: `shake(preset [, options])`, `shakeAt(position, radius, preset [, options])`,
  `addTrauma(amount)`, `stopAll([fade])`, `getInstanceCount()`;
- read/write properties: `trauma`, `enabled`, `global`, `unscaledTime`, `mode` (`"auto"`, `"3d"`,
  `"2d"`), `positionScale`, `rotationScale`, `startPreset`, `traumaDecay`, `traumaPower`,
  `traumaMaxOffset`, `traumaMaxAngle`, `traumaFrequency`;
- read-only properties: `offsetPosition` (Vector3) and `offsetRotation` (Vector3, Euler degrees).
  These are the offset applied this frame.

**`ShakeInstance`** (a handle):
- `stop([fade])`: fades out over `fade`, or `fade_out`;
- `pause([fade])` and `resume([fade])`: the noise clock slows to a stop over `fade`, and the offset
  holds;
- `isPaused()`, `isFinished()`, `isValid()`;
- read/write `strengthScale` and `roughnessScale`;
- read-only `state`: `"fading_in"`, `"sustained"`, `"fading_out"`, `"paused"`, `"stopped"`.

On a grouped handle from a static call, setters apply to all members and getters read the first
live one.

**`CameraFollow`** (phase 2; `GameObject:getCameraFollow()`):
- read/write properties: `target` (a GameObject or nil), `offset`, `offsetSpace` (`"world"`,
  `"target"`, `"target_yaw"`), `damping` (Vector3), `maxSpeed`, `lookAt`, `lookAtOffset`,
  `rotationDamping`, `deadZone` (Vector2), `useBounds`, `boundsMin`, `boundsMax`, `unscaledTime`,
  `enabled`;
- `snap()`.

C++ mirrors these in `engine/core/object/CameraShaker.h` / `CameraFollow.h`, plus the static
`CameraShake` in `engine/core/camera/CameraShake.h`.

Porting `CameraShaker.cs`:

```lua
function Hazard:onExplode()
    CameraShake.shake("explosion")
end
function Rush:start()  self.heavy = CameraShake.shake("earthquake")  end
function Rush:finish() if self.heavy then self.heavy:stop(0.2) end end
```

## 9. Editor UX

- **Inspector, Camera Shaker** (`editor/window/InspectorWindowCamera.cpp`, new, next to the Camera
  section). Rows:
  - Global, Unscaled Time, Mode, Position Scale, Rotation Scale;
  - Start Preset: a combo of the built-ins plus an asset field for `.shake`;
  - a **Trauma** foldout: Decay, Power, Max Offset, Max Angle, Frequency.
- **Test strip.** It has a preset combo, a **Shake** button, **Add Trauma 0.5**, and **Stop**.
  - In play mode it acts on the running shaker.
  - In edit mode it previews in the **Game view** through `previewShake`, and the Game view tab shows
    a "Shake preview" badge.
  - While anything is active, a live readout shows the trauma bar, the instance count, and the offset
    (position and rotation).
  - On a GameObject without a camera, a note reads "Shakes this GameObject's Transform (additive);
    preview runs in Play mode".
- **Shake Preset asset Inspector.** It shows the fields with units in the labels (units, degrees),
  the type combo, and **Load Built-in ▸**.
  - A **graph** samples the preset offline for `fade_in + 1 s + fade_out` (or 3 s) at 60 Hz with
    seed 0. It draws position x/y/z and rotation x/y/z curves, with a toggle between Position and
    Rotation, and redraws on every edit.
  - Edits are project undo steps (the clip asset rules). **Test** shakes the scene's main camera in
    the Game view (the edit-mode preview, or live in play mode).
- **Camera Follow** (phase 2) Inspector:
  - Target is an entity field, with "None" in yellow;
  - Offset, Offset Space;
  - Damping, Max Speed;
  - Look At with its offset, Rotation Damping;
  - Dead Zone, shown only for 2D cameras, and drawn in the Scene view as a rectangle gizmo;
  - Bounds: a toggle plus min and max, drawn as a box gizmo;
  - a **Snap Now** button, which in edit mode places the camera as one scene undo step;
  - a warning row when another component or script also writes the camera Transform in play mode,
    as detected by the system.
- **Component menu.** Add Component > (Camera's category) > **Camera Shaker** and **Camera Follow**.
  - The GameObject > Camera menu gets **Add Camera Shaker to Main Camera**.
  - The display names are "Camera Shaker" and "Camera Follow". The Catalog keys are `camerashaker`
    and `camerafollow`.

## 10. MCP and agent surfaces

- **Components.** `add_component` / `set_properties` handle `camerashaker` and `camerafollow` through
  the Catalog. `camerafollow.target` takes an entity reference, as other entity properties do.
  Validation:
  - `startPreset` must name a built-in or an existing `.shake`;
  - `traumaPower` must be in [1, 4];
  - scales must be ≥ 0.
- **Runtime.** `get_runtime_entity` shows a read-only `camerashaker.runtime: {instances: [{preset,
  state, strength}], trauma, offset_position, offset_rotation}` and `camerafollow.runtime: {velocity,
  desired}`.
- **Presets.** `write_file` validates `.shake` files and refuses unreadable text, returning the
  warnings. `get_asset_info` on a `.shake` shows its values plus `peak: {position, rotation}`, the
  maxima of the sampled graph. `list_assets {type: "shake"}`.
- **Testing.** `eval_lua` in play mode, for example `CameraShake.shake('explosion')`. Then
  `screenshot {view: "game"}` or `get_runtime_entity`.
- **Guide.** A new topic, `docs/mcp/guide/camera.md`, published as `jasper://docs/camera`. It covers
  shake, presets, trauma, follow and the frame order. The `jasper://guide` text gets one line. The
  tool definitions do not change: the Catalog-driven tools cover the components.

## 11. Exported-build path

- All runtime code is engine-side.
- Both components go through Stream and Factory. The C++ code generated for scenes emits their
  fields.
- `.shake` files ship with the assets (`Exporter::copyAssets`). Built-ins are compiled in.
- `Engine::getUnscaledDeltatime()` exists on every platform: it is the frame time before `timeScale`
  (Engine.cpp:1346-1356).
- No platform-specific code.

## 12. Behaviour rules and edge cases

- **S1. Script-driven cameras are never fought.** A camera moved by a script, or by Camera Follow,
  every frame shakes around its moving pose. Lua reads of `position` and `rotation` are never shaken.
- **S2. Two shakers on one camera are impossible** (one component per type). Two global shakers on
  two cameras of a split screen each get their own instance from `CameraShake.shake`.
- **S3. Camera switch.** `Camera:activate()` on another camera: each shaker shakes its own camera.
  Static calls shake every global shaker, active camera or not. Only the drawn view shows it.
- **S4. `useTarget` cameras** (look-at by default). The offset is applied after the look-at view, so
  shaking works the same.
- **S5. Render-to-texture cameras** shake when they have a shaker. Mirror cameras derive from the
  shaken main view. That is consistent.
- **S6. Zero strength.** A strength of 0 or a scale of 0 produces a zero offset. `hasViewOffset` is
  false then, and the matrices are not recomputed.
- **S7. Scene load.** Instances die with their shaker (decision 6). Static handles from a destroyed
  scene become invalid. Trauma is per shaker and does not carry across loads.
- **S8. Transform mode and other writers.** In transform mode, a script that writes the Transform
  every frame becomes the new base each frame. The shake is still visible and additive. An Animation
  Player writing that Transform works the same way, because the flush runs before the system.
- **S9. Follow edge cases.** A follower whose target is destroyed stops moving. Its reference
  resolves to none, with a warning once. A target in another scene is not supported (an editor
  warning). `deadZone` is ignored in 3D perspective mode, with a note in the Inspector.
- **S10. Physics interpolation.** Targets interpolated by the physics system (Engine.cpp:1412-1418)
  are followed at their interpolated pose, because the system runs after interpolation.

## 13. Performance (mobile)

- **Per shaker per frame:** up to 32 instances × 6 Perlin evaluations, plus trauma (6 evaluations),
  plus one matrix product in `updateCamera` while the shake is active.
- **Budget:** 4 shakers with 8 active instances each must cost ≤ 0.05 ms on a mid-range Android
  device. A host test asserts ≤ 0.02 ms in a Release build. Idle shakers cost nothing: there is an
  early-out when there are no instances and trauma is 0.
- **Follow:** O(depth of the target) for `computeWorldNow`, plus a SmoothDamp. Negligible.

## 14. Tests

Engine tests: a new `engine/tests/camera` target, `jasperengine-camera-tests`, built like the anim
tests (`-DJASPERENGINE_CAMERA_TESTS=ON`). It covers pure maths and a headless scene.

1. **Noise.** `perlinNoise` is in [0, 1] over 10⁵ samples, gives 0.5 at integer lattice points, and
   is continuous (|Δ| < 0.05 for a step of 0.01).
2. **Fade timeline.** A one-shot with `fade_in` 0.05 and `fade_out` 1, stepped at 1/60:
   - fade reaches 1 at 0.05 s (±1 step), then decreases;
   - the instance is removed at 1.05 s (±1 step);
   - a sustained shake holds fade = 1 until `stop(0.5)`, then is removed 0.5 s later;
   - `fade_in` 0 jumps to 1 on the first step.
3. **MilkShake formula.** With seed `b = 100` and a fixed perlin, at fade 1 and `t = 100.5`, the
   position x equals `(perlin(100.5 + 50, 100) - 0.5) * 2 * strength * influence.x` (1e-6). Rotation z
   equals `N(100 + 100.5, 50 + 100.5) * strength * influence.z`.
4. **Scales.** `strengthScale` 0.5 halves the offsets. `roughnessScale` 2 doubles the clock advance.
   `shakeAt` at d = radius/2 gives 0.5, and at d ≥ radius it creates no instance.
5. **Trauma.** `addTrauma(0.5)` twice clamps to 1. Decay 1/s gives 0.5 after 0.5 s. At trauma 0.5
   with power 2, the amplitude factor is 0.25.
6. **The view offset leaves the Transform alone.** A camera at position (0, 0, 10) with a shaker
   (position offset forced to (1, 0, 0), rotation 0):
   - after `RenderSystem::update`, `camera.viewMatrix` equals the view of a camera at (1, 0, 10)
     (1e-5);
   - `Transform.position` is still (0, 0, 10);
   - a script setting the position to (5, 0, 10) gives a view at (6, 0, 10) the same frame.
7. **2D.** An ortho camera with `topClip - bottomClip = 20` and a preset position influence of (1, 1, 1)
   with strength 1: the max x offset is ≤ 2 units (20/10 × |N| ≤ 1), z is 0, and rotation x/y are 0.
8. **Scaled vs unscaled.** At `timeScale = 0`, a scaled shake's clock does not advance and its offset
   is constant. An unscaled one advances. Its offset differs after 0.1 s of frames.
9. **Lifetime.** Destroying the camera GameObject invalidates its handle (`isValid` false;
   `stop` is a no-op). Disabling the shaker clears `hasViewOffset`. Enabling it again replays
   `startPreset`.
10. **Transform mode.** On a UI Image with anchors, the shake offsets `positionOffset` and the layout
    keeps it. When the shake ends, the original `positionOffset` is restored exactly. A script write
    during the shake becomes the base (S8).
11. **Follow (phase 2).**
    - With damping 0.2 and a target stepping 1 unit at t = 0, after 0.2 s the camera has covered the
      SmoothDamp-reference fraction (an exact value from the Unity formula, 1e-4).
    - Damping 0 snaps.
    - A dead zone (2, 2) and a target move of 0.9 in x leave the camera still. A move of 1.5 moves it
      0.5 (rigid).
    - Bounds clamp.
    - `useTarget` cameras get `camera.target` set to the look point.
    - The current-frame world position is used: a target moved in `onUpdate` is followed the same
      frame.

Editor tests (`editor/tests`):
- Stream round trip of both components.
- The Factory emitter compiles in the generator test.
- `.shake` asset Inspector edit, then undo.
- `previewShake` in edit mode leaves the scene unmodified, and the view offset is cleared on stop.

MCP tests (`tools/mcp/tests/test_camera_shake.py`):
- `add_component camerashaker`, `write_file` of a `.shake`;
- play, then `eval_lua "CameraShake.shake('assets/shakes/boom.shake')"`;
- `get_runtime_entity` shows one instance and a non-zero offset, then zero after the fade;
- the camera follow flow, checked with `get_runtime_entity` on the camera position;
- `get_asset_info` gives the peaks.

## 15. Acceptance criteria

- [ ] Tests 1–11 and the editor and MCP tests pass. A scene without these components renders
  identically: `hasViewOffset` is false and there is no matrix change.
- [ ] The game's 13 presets are portable by name or as `.shake` files with the same numbers.
  `explosion` on a following camera in a 3D scene and `jumpscare_2d` on an ortho camera look right in
  the editor and in an exported Linux build.
- [ ] Shakes never write the camera Transform. Static calls work across child scenes. Unscaled shakes
  run at `timeScale` 0.
- [ ] The Inspector Test works in edit mode (Game view) and in play mode. The preset graph updates
  live.
- [ ] Docs:
  - `docs/mcp/guide/camera.md` (new), plus a user-facing section in a new `docs/camera.md`;
  - `lua-api.md`: a new subsection 4.9.1, CameraShake / CameraShaker / ShakeInstance / CameraFollow;
  - `docs/unity-glossary.md`: MilkShake Shaker / ShakePreset / ShakeInstance, Cinemachine Impulse
    Source/Listener, Cinemachine follow → Camera Follow, `Mathf.SmoothDamp` note;
  - the README feature list.

  The Lua stubs have no unresolved entries.
- [ ] Performance meets section 13.

## 16. Integration points

- `engine/core/component/CameraComponent.h` (view offset fields, runtime-only);
  `engine/core/subsystem/RenderSystem.cpp:11129-11182` (`updateCamera`: apply after the view matrix,
  skip when `hasCustomViewMatrix`), and `:13273-13280` (the camera update loop, where `needUpdate` is
  honoured).
- `engine/core/Scene.cpp:38-50` (register `CameraControlSystem` before `UISystem`).
- `engine/core/Engine.{h,cpp}`: `getUnscaledDeltatime()` (decision 3; the frame time computed at
  Engine.cpp:1346-1356).
- New files:
  - `engine/core/subsystem/CameraControlSystem.{h,cpp}`;
  - `engine/core/camera/{ShakePreset,ShakePresets,ShakeInstance,CameraShake}.{h,cpp}`;
  - `engine/core/math/Noise.h`;
  - `engine/core/pool/ShakePresetPool.{h,cpp}` (registered in `Engine::clearPools`);
  - `engine/core/component/{CameraShakerComponent,CameraFollowComponent}.h`;
  - `engine/core/object/{CameraShaker,CameraFollow}.{h,cpp}`.
- `engine/core/subsystem/ActionSystem.cpp:1650-1658` (the anchored-UI `positionOffset` rule, reused
  in transform mode).
- `engine/core/pose/PoseAnimatorRuntime.h:46-60` (the PoseBlendBone base-detection rule, reused).
- Lua bindings: `engine/core/script/binding/GameObjectClassesLua.cpp` (the wrapper classes and
  `getCameraShaker` / `getCameraFollow` near :517), `CoreClassesLua.cpp` (the static `CameraShake`).
- Editor:
  - `editor/Catalog.{h,cpp}` (`ComponentType` :146-152, names, display names, icons, properties);
  - `editor/Stream.cpp`, `editor/Factory.cpp`;
  - `editor/window/InspectorWindow.cpp` (section dispatch near :16143, Add Component);
  - `editor/window/InspectorWindowCamera.cpp` (new);
  - the Project window's Create menu (Shake Preset);
  - the asset type registry (`.shake` as an asset type for `list_assets` and `get_asset_info`);
  - `editor/mcp/tools/AssetTools.cpp` (`get_asset_info`), `PlayTools.cpp` (`get_runtime_entity`
    runtime block).

## 17. Cross-feature dependencies

- **`coroutines` (#16)** introduces `Engine::getUnscaledDeltatime()` and the Lua `Engine.unscaledDeltatime`.
  If `camera-shake` merges first, it adds that C++ getter with the same name and semantics: the frame
  time before `timeScale`, and 0 while the simulation is held. `coroutines` then adds only the Lua
  binding. The ledger lead resolves the order.
- **`persistence` (#20).** A persistent (DontDestroyOnLoad) camera keeps its shaker and instances
  across loads, because they live in the component. Nothing else is needed.
- **`debug-overlay` (#23)** is unrelated. It may show `CameraShake` stats later.
- **The animation features** are independent. A camera animated by an Animation Player still shakes,
  because the shake is a view offset.
